from pathlib import Path

from s3.config import Config, ConfigError

FULL = {"EVAL_S3_ENDPOINT": "http://127.0.0.1:9000", "EVAL_S3_BUCKET": "grparse-eval", "EVAL_S3_PREFIX": "gRParse/",
        "EVAL_S3_ACCESS_KEY": "k", "EVAL_S3_SECRET_KEY": "s", "EVAL_S3_MAX_OBJECTS": "5",
        "EVAL_S3_INCLUDE": "*.pdf, *.docx", "EVAL_S3_EXCLUDE": "*.warc*", "GRPARSE_TARGET": "127.0.0.1:50051",
        "EVAL_OUT": "/tmp/out", "EVAL_LABEL": "verdict", "EVAL_S3_REPEAT": "3", "EVAL_REQUIRE": "1"}


def test_missing_names_are_listed() -> None:
    try:
        Config.from_env({"EVAL_S3_ENDPOINT": "x"}, Path("/repo"))
    except ConfigError as error:
        assert "EVAL_S3_BUCKET" in str(error) and "AWS_ACCESS_KEY_ID" in str(error)
    else:
        raise AssertionError("missing bucket and credentials must raise")


def test_full_environment_parses() -> None:
    config = Config.from_env(FULL, Path("/repo"))
    assert config.bucket == "grparse-eval" and config.prefix == "gRParse/"
    assert config.include == ("*.pdf", "*.docx") and config.exclude == ("*.warc*",)
    assert config.max_objects == 5 and config.repeat == 3 and config.require
    assert config.out == Path("/tmp/out") and config.label == "verdict"
    assert config.region == "us-east-1"


def test_aws_names_and_defaults() -> None:
    env = {"EVAL_S3_ENDPOINT": "http://h:9000", "EVAL_S3_BUCKET": "b", "AWS_ACCESS_KEY_ID": "a",
           "AWS_SECRET_ACCESS_KEY": "s"}
    config = Config.from_env(env, Path("/repo"))
    assert config.out == Path("/repo/eval/out") and config.max_objects is None and config.repeat == 2
    assert config.sniff_per_extension == 1 and not config.require and config.target == "localhost:50051"
    assert config.convert_timeout == 600.0
    assert Config.from_env(dict(env, EVAL_S3_CONVERT_TIMEOUT="45"), Path("/repo")).convert_timeout == 45.0
    # A long document gets 1.5 s a page past the flat deadline; an unknown
    # page count keeps the flat deadline.
    assert config.deadline_for(0) == 600.0 and config.deadline_for(100) == 600.0
    assert config.deadline_for(955) == 1432.5
    paced = Config.from_env(dict(env, EVAL_S3_SECONDS_PER_PAGE="2"), Path("/repo"))
    assert paced.deadline_for(955) == 1910.0


def test_bad_integer_is_named() -> None:
    env = dict(FULL, EVAL_S3_MAX_OBJECTS="many")
    try:
        Config.from_env(env, Path("/repo"))
    except ConfigError as error:
        assert "EVAL_S3_MAX_OBJECTS" in str(error)
    else:
        raise AssertionError("a non-integer limit must raise")


def test_public_endpoint_drops_userinfo() -> None:
    config = Config.from_env(dict(FULL, EVAL_S3_ENDPOINT="http://user:pw@host:9000"), Path("/repo"))
    assert config.public_endpoint() == "host:9000"


def test_password_file_is_read_and_never_printed() -> None:
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "passwords.txt"
        path.write_text("first-secret\n\n  \nsécond secret\n", encoding="utf-8")
        config = Config.from_env(dict(FULL, EVAL_S3_PASSWORDS_FILE=str(path)), Path("/repo"))
        assert config.document_passwords == ("first-secret", "sécond secret")
        assert "first-secret" not in repr(config) and "cond secret" not in repr(config)
        assert Config.from_env(FULL, Path("/repo")).document_passwords == ()

        path.write_text("\n".join(f"secret-{i}" for i in range(17)), encoding="utf-8")
        try:
            Config.from_env(dict(FULL, EVAL_S3_PASSWORDS_FILE=str(path)), Path("/repo"))
        except ConfigError as error:
            assert "at most 16" in str(error) and "secret-" not in str(error)
        else:
            raise AssertionError("more candidates than gRParse takes must raise")

    try:
        Config.from_env(dict(FULL, EVAL_S3_PASSWORDS_FILE="/nonexistent/passwords.txt"), Path("/repo"))
    except ConfigError as error:
        assert "EVAL_S3_PASSWORDS_FILE" in str(error)
    else:
        raise AssertionError("an unreadable password file must raise")


def test_concurrency_defaults_to_one_and_is_bounded() -> None:
    assert Config.from_env(dict(FULL), Path("/repo")).concurrency == 1
    assert Config.from_env(dict(FULL, EVAL_S3_CONCURRENCY="4"), Path("/repo")).concurrency == 4
    try:
        Config.from_env(dict(FULL, EVAL_S3_CONCURRENCY="17"), Path("/repo"))
    except ConfigError as error:
        assert "EVAL_S3_CONCURRENCY" in str(error)
    else:
        raise AssertionError("a concurrency over the bound must raise")
