# Homebrew formula for CWIST. Maintained here and mirrored into a tap
# repository (e.g. Religiya-Serdtsa/homebrew-cwist) as Formula/cwist.rb.
#
# Before publishing a release:
#   1. make dist
#   2. upload dist/cwist-<version>.tar.gz to the GitHub release assets
#   3. update `url` and `sha256` below
class Cwist < Formula
  desc "C17 web framework and application server (HTTP/1.1, HTTP/2, HTTP/3, WebSocket, PQC TLS)"
  homepage "https://github.com/Religiya-Serdtsa/CWIST"
  url "https://github.com/Religiya-Serdtsa/CWIST/releases/download/v3.2/cwist-3.2.tar.gz"
  sha256 "0000000000000000000000000000000000000000000000000000000000000000" # fill with `shasum -a 256 dist/cwist-3.2.tar.gz`
  license "MIT"

  depends_on "cmake" => :build
  depends_on "pkg-config" => :build
  depends_on "brotli"
  depends_on "zstd"

  uses_from_macos "zlib"
  uses_from_macos "curl"

  def install
    system "make", "-j#{ENV.make_jobs}"
    system "make", "install", "PREFIX=#{prefix}"
  end

  test do
    (testpath/"test.c").write <<~EOS
      #include <cwist/app.h>
      static void hello(cwist_http_request *req, cwist_http_response *res) {
          (void)req;
          cwist_sstring_assign(res->body, "ok");
      }
      int main(void) {
          cwist_app *app = cwist_app_create();
          cwist_app_get(app, "/", hello);
          cwist_app_destroy(app);
          return 0;
      }
    EOS
    flags = shell_output("pkg-config --cflags --libs cwist").split
    system ENV.cc, "-o", "test", "test.c", *flags
    system "./test"
  end
end
