# serve.py PORT [FOLDER]: `python3 -m http.server PORT --bind 127.0.0.1`,
# printing the same line, without the reverse DNS lookup its server_bind
# makes. HTTPServer asks socket.getfqdn("127.0.0.1") between bind() and
# listen(), and on GitHub's macOS runners that lookup (a PTR query for
# 1.0.0.127.in-addr.arpa) can take longer than the checks wait, so the port
# was not listening and the address not printed in time.
import functools
import http.server
import socketserver
import sys


class Server(http.server.ThreadingHTTPServer):
    def server_bind(self):
        socketserver.TCPServer.server_bind(self)
        self.server_name = "localhost"
        self.server_port = self.server_address[1]


def main():
    port = int(sys.argv[1])
    folder = sys.argv[2] if len(sys.argv) > 2 else "."
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=folder)
    with Server(("127.0.0.1", port), handler) as httpd:
        print(f"Serving HTTP on 127.0.0.1 port {port} (http://127.0.0.1:{port}/) ...", flush=True)
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
