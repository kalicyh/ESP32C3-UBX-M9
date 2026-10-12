#!/usr/bin/env python3
"""Serve the static BLE dashboard. No telemetry or Bluetooth proxy runs here."""
import argparse
import functools
import pathlib
import ssl
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

p=argparse.ArgumentParser()
p.add_argument('--host',default='127.0.0.1')
p.add_argument('--port',type=int,default=8765)
p.add_argument('--cert',type=pathlib.Path,help='Trusted TLS certificate for HTTPS clients')
p.add_argument('--key',type=pathlib.Path,help='TLS private key')
args=p.parse_args()
if bool(args.cert)!=bool(args.key): p.error('--cert and --key must be supplied together')
root=pathlib.Path(__file__).resolve().parents[1]/'main'/'web'
class Handler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cache-Control','no-store')
        self.send_header('Permissions-Policy','bluetooth=(self)')
        super().end_headers()
server=ThreadingHTTPServer((args.host,args.port),functools.partial(Handler,directory=str(root)))
protocol='http'
if args.cert:
    ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(args.cert,args.key)
    server.socket=ctx.wrap_socket(server.socket,server_side=True)
    protocol='https'
print(f'BLE dashboard: {protocol}://{args.host}:{args.port}/',flush=True)
if args.host not in ('127.0.0.1','localhost') and not args.cert:
    print('Remote HTTP clients cannot use Web Bluetooth. Use trusted HTTPS for Android.',flush=True)
server.serve_forever()
