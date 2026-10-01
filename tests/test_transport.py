#!/usr/bin/env python3
"""Isolated loopback TLS server: sends both PDUs in one record, then stays silent."""
import socket, ssl, subprocess, tempfile, threading
from pathlib import Path
H=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory() as td:
    cert=Path(td)/'cert'; key=Path(td)/'key'
    subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(key),
                    '-out',str(cert),'-days','1','-subj','/CN=localhost'],check=True,capture_output=True)
    ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version=ctx.maximum_version=ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(cert,key)
    with socket.socket() as listener:
        listener.bind(('127.0.0.1',0)); listener.listen(1); listener.settimeout(20)
        finished=threading.Event(); errors=[]
        def serve():
            try:
                conn,_=listener.accept()
                with ctx.wrap_socket(conn,server_side=True) as tls:
                    tls.sendall(bytes.fromhex('00050300000005030000'))
                    finished.wait(20)
            except Exception as e: errors.append(e)
        thread=threading.Thread(target=serve); thread.start()
        try:
            subprocess.run(['python3',str(H/'run_arm.py'),str(H/'test_transport.c'),
                            str(listener.getsockname()[1])],check=True,timeout=20)
        finally:
            finished.set(); thread.join()
        assert not errors,errors
