// A minimal S3 object writer over libcurl: one signed PUT per object, path
// style, against any S3-compatible endpoint.  No AWS SDK, because a PUT with
// a Signature V4 header is the whole of what the S3 target needs, and no
// vendored credential chain either: the credentials arrive in the request and
// live no longer than the call.
//
// Nothing here ever renders a credential.  Errors name the object key and the
// transport failure and stop there, because an error message travels: it goes
// into an RPC status, and from there into whatever the caller logs.
#ifndef GRPARSE_TARGETS_S3_CLIENT_H
#define GRPARSE_TARGETS_S3_CLIENT_H

#include <string>

namespace grparse::targets {

// Where the objects go and who signs for them.  A copy of the request's own
// S3Target, kept for exactly the length of one conversion.
struct S3Config {
  // Endpoint with or without a scheme; no scheme means https.
  std::string endpoint;
  std::string access_key;
  std::string secret_key;
  std::string bucket;
  std::string key_prefix;
  bool verify_ssl = true;
};

// The region named by an endpoint host ("s3.eu-west-1.amazonaws.com" ->
// "eu-west-1"), or "us-east-1" when the host names none, which is what every
// S3-compatible store that does not care about regions accepts.
std::string region_for_endpoint(const std::string& endpoint);

// One object PUT.  Returns the store's ETag for the written object, or an
// empty string when the store returned none.  Throws std::runtime_error
// naming the key on any transport or HTTP failure; the store's response body
// is deliberately not part of that message, since an S3 error document can
// quote the access key back.
class S3Client final {
 public:
  explicit S3Client(S3Config config);

  std::string put_object(const std::string& key, const std::string& body) const;

  // The full key for a bundle member: the configured prefix and the member's
  // path, joined by a single separator.
  std::string key_for(const std::string& path) const;

 private:
  S3Config config_;
  std::string scheme_;
  // Host with its port, as it goes on the wire and into the signature.
  std::string host_header_;
  // Authority for the URL, which keeps the port libcurl needs to dial even
  // where the Host header drops it.
  std::string authority_;
  // Any path the endpoint itself carried, ahead of the bucket.
  std::string base_path_;
  std::string region_;
};

}  // namespace grparse::targets

#endif
