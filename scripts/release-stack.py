#!/usr/bin/env python3
"""Checks and resolves the image set of a fleet release (docs/RELEASING.md).

The fleet is release/fleet.tsv: every repository a release tags and the
images its publish workflow pushes for a v<version> tag. The compose stack
(compose.stack*.yaml) pins every pipestreamai/* image with one variable,
STACK_TAG, so a release is only usable when each of those images carries
the version tag. This script holds the two to each other.

  check                      every ${STACK_TAG} image in the compose stack
                             and every ${GRPARSE_TAG} image in
                             quickstart/compose.yaml has a publisher in
                             fleet.tsv, and fleet.tsv parses; no network
  images --version V         prints the fleet's image references for V
  resolve --version V        looks every fleet image up on Docker Hub and
                             writes the digest lock; exits 1 naming each
                             tag that does not exist. --wait SECONDS keeps
                             polling (every --interval seconds) until all
                             tags exist or the time is up, since the
                             parser publishes finish at different times.
  git-tags --version V       checks that every fleet repository on GitHub
                             has the tag v<V> and prints the commit each
                             one points at (GITHUB_TOKEN, if set, is used
                             for the API calls)

Only the Python standard library is used, so the script runs on a bare
runner and on krick alike.
"""

import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FLEET = ROOT / "release" / "fleet.tsv"
NAMESPACE = "pipestreamai"
GITHUB_OWNER = "ai-pipestream"
SEMVER = re.compile(
    r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z][0-9A-Za-z.-]{0,63})?$"
)
# An image template is <name>:{v}<suffix>, the suffix a variant such as -cpu.
TEMPLATE = re.compile(r"^[a-z0-9][a-z0-9-]*:\{v\}(-[a-z0-9]+)?$")
# The compose files that pin pipestreamai/* images, each with its own tag
# variable: the stack with STACK_TAG, the quickstart with GRPARSE_TAG.
PINNED_COMPOSE = (("compose.stack*.yaml", "STACK_TAG"), ("quickstart/compose.yaml", "GRPARSE_TAG"))


def pinned_image(variable):
    return re.compile(
        r"^\s*image:\s*[\"']?" + NAMESPACE + r"/([a-z0-9][a-z0-9-]*):\$\{" + variable
        + r":-latest\}([-a-z0-9]*)[\"']?\s*$"
    )
MANIFEST_TYPES = ", ".join([
    "application/vnd.oci.image.index.v1+json",
    "application/vnd.docker.distribution.manifest.list.v2+json",
    "application/vnd.oci.image.manifest.v1+json",
    "application/vnd.docker.distribution.manifest.v2+json",
])


def fail(message):
    print(f"release-stack: {message}", file=sys.stderr)
    sys.exit(1)


def load_fleet(path=FLEET):
    """Returns [(repo, branch, [template, ...])] in file order."""
    fleet = []
    seen = set()
    for number, line in enumerate(path.read_text().splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        fields = line.split()
        if len(fields) < 3:
            fail(f"{path.name}:{number}: want 'repo branch image...', got {line!r}")
        repo, branch, templates = fields[0], fields[1], fields[2:]
        for template in templates:
            if not TEMPLATE.match(template):
                fail(f"{path.name}:{number}: {template!r} is not <name>:{{v}}[-variant]")
            if template in seen:
                fail(f"{path.name}:{number}: {template} is listed twice")
            seen.add(template)
        fleet.append((repo, branch, templates))
    if not fleet:
        fail(f"{path} lists no repositories")
    return fleet


def stack_templates(root=ROOT):
    """Every tag-pinned image in the compose stack and the quickstart, as fleet templates."""
    found = {}
    for pattern, variable in PINNED_COMPOSE:
        image = pinned_image(variable)
        for compose in sorted(root.glob(pattern)):
            name = compose.relative_to(root)
            for number, line in enumerate(compose.read_text().splitlines(), 1):
                match = image.match(line)
                if match:
                    template = f"{match.group(1)}:{{v}}{match.group(2)}"
                    found.setdefault(template, f"{name}:{number}")
                elif re.match(rf"^\s*image:\s*[\"']?{NAMESPACE}/", line):
                    fail(f"{name}:{number}: a {NAMESPACE} image not pinned by "
                         f"${{{variable}:-latest}}: {line.strip()}")
    return found


def render(template, version):
    return f"{NAMESPACE}/{template.replace('{v}', version)}"


def cmd_check(_args):
    fleet = load_fleet()
    published = {t for _, _, templates in fleet for t in templates}
    stack = stack_templates()
    if not stack:
        fail("found no tag-pinned images in compose.stack*.yaml or quickstart/compose.yaml")
    orphans = sorted(set(stack) - published)
    for template in orphans:
        print(f"release-stack: {stack[template]}: {render(template, '<version>')} "
              "has no publisher in release/fleet.tsv", file=sys.stderr)
    if orphans:
        sys.exit(1)
    unused = sorted(published - set(stack))
    print(f"release-stack: {len(stack)} stack images, all published by the "
          f"{len(fleet)} fleet repositories")
    for template in unused:
        print(f"  also released, not in the stack: {render(template, '<version>')}")


def version_arg(value):
    if not SEMVER.match(value):
        raise argparse.ArgumentTypeError(f"{value!r} is not a semantic version such as 0.3.0")
    return value


def cmd_images(args):
    for _, _, templates in load_fleet():
        for template in templates:
            print(render(template, args.version))


def http(url, headers=None, method="GET", attempts=4):
    """One request; a 429, a 5xx or a network error is retried with backoff."""
    request = urllib.request.Request(url, headers=headers or {}, method=method)
    for attempt in range(1, attempts + 1):
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                return response.status, dict(response.headers), response.read()
        except urllib.error.HTTPError as error:
            if attempt == attempts or (error.code != 429 and error.code < 500):
                raise
        except urllib.error.URLError:
            if attempt == attempts:
                raise
        time.sleep(2 ** attempt)


def hub_digest(reference):
    """The manifest digest of name:tag on Docker Hub, or None if the tag is absent.

    A HEAD on the manifest does not count against the anonymous pull limit.
    """
    repository, tag = reference.rsplit(":", 1)
    _, _, body = http("https://auth.docker.io/token?service=registry.docker.io"
                      f"&scope=repository:{repository}:pull")
    token = json.loads(body)["token"]
    try:
        _, headers, _ = http(
            f"https://registry-1.docker.io/v2/{repository}/manifests/{tag}",
            {"Authorization": f"Bearer {token}", "Accept": MANIFEST_TYPES},
            method="HEAD",
        )
    except urllib.error.HTTPError as error:
        if error.code == 404:
            return None
        raise
    digest = {k.lower(): v for k, v in headers.items()}.get("docker-content-digest")
    if not digest:
        fail(f"Docker Hub returned no digest for {reference}")
    return digest


def cmd_resolve(args):
    fleet = load_fleet()
    wanted = [(repo, render(t, args.version)) for repo, _, templates in fleet for t in templates]
    deadline = time.monotonic() + args.wait
    digests = {}
    while True:
        for _, reference in wanted:
            if reference not in digests:
                digest = hub_digest(reference)
                if digest:
                    digests[reference] = digest
        missing = [(repo, ref) for repo, ref in wanted if ref not in digests]
        if not missing or time.monotonic() + args.interval > deadline:
            break
        print(f"release-stack: {len(missing)} of {len(wanted)} tags not published yet "
              f"({', '.join(ref for _, ref in missing[:4])}{', ...' if len(missing) > 4 else ''}); "
              f"checking again in {args.interval}s", flush=True)
        time.sleep(args.interval)

    lines = [f"{ref}@{digests[ref]}" for _, ref in wanted if ref in digests]
    if args.lock:
        Path(args.lock).write_text("".join(f"{line}\n" for line in lines))
    if args.markdown:
        rows = ["| Repository | Image | Digest |", "|---|---|---|"]
        for repo, ref in wanted:
            rows.append(f"| {repo} | `{ref}` | {'`' + digests[ref] + '`' if ref in digests else '**missing**'} |")
        Path(args.markdown).write_text("\n".join(rows) + "\n")
    for line in lines:
        print(line)
    for repo, ref in missing:
        print(f"release-stack: {ref} ({repo}) is not on Docker Hub", file=sys.stderr)
    if missing:
        sys.exit(1)


def cmd_git_tags(args):
    headers = {"Accept": "application/vnd.github+json"}
    if os.environ.get("GITHUB_TOKEN"):
        headers["Authorization"] = f"Bearer {os.environ['GITHUB_TOKEN']}"
    api = "https://api.github.com/repos"
    tag = f"v{args.version}"
    missing = []
    rows = ["| Repository | Branch | Tag commit |", "|---|---|---|"]
    for repo, branch, _ in load_fleet():
        try:
            _, _, body = http(f"{api}/{GITHUB_OWNER}/{repo}/git/ref/tags/{tag}", headers)
        except urllib.error.HTTPError as error:
            if error.code == 404:
                missing.append(repo)
                rows.append(f"| {repo} | {branch} | **no {tag}** |")
                continue
            raise
        target = json.loads(body)["object"]
        while target["type"] == "tag":  # annotated: follow the tag object
            _, _, body = http(f"{api}/{GITHUB_OWNER}/{repo}/git/tags/{target['sha']}", headers)
            target = json.loads(body)["object"]
        sha = target["sha"]
        rows.append(f"| {repo} | {branch} | [`{sha[:12]}`](https://github.com/{GITHUB_OWNER}/{repo}/commit/{sha}) |")
        print(f"{repo} {tag} {sha}")
    if args.markdown:
        Path(args.markdown).write_text("\n".join(rows) + "\n")
    for repo in missing:
        print(f"release-stack: {GITHUB_OWNER}/{repo} has no tag {tag} on GitHub", file=sys.stderr)
    if missing:
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("check").set_defaults(run=cmd_check)
    images = commands.add_parser("images")
    images.add_argument("--version", type=version_arg, required=True)
    images.set_defaults(run=cmd_images)
    resolve = commands.add_parser("resolve")
    resolve.add_argument("--version", type=version_arg, required=True)
    resolve.add_argument("--wait", type=int, default=0, metavar="SECONDS")
    resolve.add_argument("--interval", type=int, default=60, metavar="SECONDS")
    resolve.add_argument("--lock", metavar="FILE", help="write image@digest lines here")
    resolve.add_argument("--markdown", metavar="FILE", help="write a markdown table here")
    resolve.set_defaults(run=cmd_resolve)
    tags = commands.add_parser("git-tags")
    tags.add_argument("--version", type=version_arg, required=True)
    tags.add_argument("--markdown", metavar="FILE", help="write a markdown table here")
    tags.set_defaults(run=cmd_git_tags)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
