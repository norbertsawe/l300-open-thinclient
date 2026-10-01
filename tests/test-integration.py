#!/usr/bin/env python3
"""Loopback-only xrdp TLS 1.2/pinning/drawing test, isolated with bubblewrap.

Requires the optional Debian xrdp/JPEG packages extracted into test-server.
Does not start sesman, authenticate a user, or install a service.
"""
import hashlib
import os
from pathlib import Path
import signal
import socket
import subprocess
import time

B = Path(__file__).resolve().parents[2] / 'l300-build'
S = B / 'test-server'
L = B / 'logs'
checks = []
(L/'integration-summary.txt').unlink(missing_ok=True)
def passed(message):
    checks.append('PASS: ' + message)
    print(checks[-1])
env = dict(os.environ)
libs = f'{S}/usr/lib/x86_64-linux-gnu/xrdp:{S}/usr/lib/x86_64-linux-gnu'
env['LD_LIBRARY_PATH'] = libs
subprocess.run([str(S/'usr/bin/xrdp-keygen'),'xrdp',str(S/'etc/xrdp/rsakeys.ini')],
               env=env,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes',
                '-keyout',str(S/'key.pem'),'-out',str(S/'cert.pem'),'-days','1',
                '-subj','/CN=l300-loopback-test'],check=True,
               stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
config = (S/'etc/xrdp/xrdp.ini').read_text()
for before, after in [('port=3389\n','port=tcp://127.0.0.1:13389\n'),
                      ('security_layer=negotiate','security_layer=tls'),
                      ('certificate=\n',f'certificate={S}/cert.pem\n'),
                      ('key_file=\n',f'key_file={S}/key.pem\n'),
                      ('LogFile=xrdp.log',f'LogFile={S}/xrdp.log'),
                      ('EnableSyslog=true','EnableSyslog=false'),
                      ('ssl_protocols=TLSv1.2, TLSv1.3','ssl_protocols=TLSv1.2'),
                      ('max_bpp=32','max_bpp=16')]:
    config = config.replace(before, after)
(S/'test.ini').write_text(config)
(S/'xrdp.log').write_text('')
command = ['bwrap', '--ro-bind', '/', '/', '--bind', str(B), str(B),
           '--tmpfs', '/etc', '--ro-bind', '/etc/passwd', '/etc/passwd',
           '--ro-bind', str(S / 'etc/xrdp'), '/etc/xrdp',
           '--tmpfs', '/usr/share', '--ro-bind', str(S / 'usr/share/xrdp'), '/usr/share/xrdp',
           '--tmpfs', '/run', '--dir', '/run/xrdp', '--unshare-user', '--unshare-pid',
           '--die-with-parent', '--setenv', 'LD_LIBRARY_PATH', libs,
           str(S / 'usr/sbin/xrdp'), '--nodaemon', '--config', str(S / 'test.ini')]
der = subprocess.check_output(['openssl', 'x509', '-in', str(S / 'cert.pem'), '-outform', 'DER'])
pin = hashlib.sha256(der).hexdigest()
server = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
try:
    for _ in range(50):
        if server.poll() is not None:
            raise RuntimeError('xrdp failed: ' + server.stderr.read().decode())
        try:
            with socket.create_connection(('127.0.0.1', 13389), timeout=.1):
                break
        except OSError:
            time.sleep(.1)
    else:
        raise RuntimeError('xrdp did not start')
    for label, selected_pin in [('valid-pin', pin), ('wrong-pin', '00'*32), ('missing-pin', None)]:
        frame = B / 'out' / (label + '.ppm')
        frame.unlink(missing_ok=True)
        args = [str(B / 'downloads/qemu-arm-static'), '-cpu', 'arm926',
                str(B / 'out/fbfreerdp'), '--headless', '--dump-frame', str(frame),
                '--timeout', '10', '-t', '13389']
        if selected_pin:
            args += ['--appliance', '--cert-sha256', selected_pin]
        args += ['127.0.0.1']
        with (L / (label + '.log')).open('w') as log:
            client = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)
            try:
                if label == 'valid-pin':
                    time.sleep(5)
                    client.send_signal(signal.SIGTERM)
                status = client.wait(timeout=15)
            finally:
                if client.poll() is None:
                    client.kill()
                    client.wait()
        output = (L / (label + '.log')).read_text()
        if label == 'valid-pin':
            assert status == 0 and 'Connected: 1024x768' in output, (status, output)
            data = frame.read_bytes().split(b'\n', 3)[3]
            colours = {data[i:i+3] for i in range(0, len(data), 3)}
            assert len(data) == 1024*768*3 and len(colours) > 5, len(colours)
            passed(f'TLS 1.2 RDP connection, login screen rendered ({len(colours)} colours), clean exit')
        else:
            assert status == 66 and 'Certificate pin missing or different' in output, (status, output)
            assert not frame.exists()
            passed('rejected ' + label)
    server_log = (S / 'xrdp.log').read_text()
    assert 'TLSv1.2' in server_log
    passed('server confirms TLSv1.2')
    with socket.socket() as stalled:
        stalled.bind(('127.0.0.1', 0))
        stalled.listen(1)
        result = subprocess.run([str(B/'downloads/qemu-arm-static'),'-cpu','arm926',
                                 str(B/'out/fbfreerdp'),'--headless','--timeout','1',
                                 '-t',str(stalled.getsockname()[1]),'127.0.0.1'],
                                text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=5)
        (L/'timeout.log').write_text(result.stdout)
        assert result.returncode == 124 and 'timed out' in result.stdout, result
        passed('stalled connection terminates with status 124')
    digest = hashlib.sha256((B/'out/fbfreerdp').read_bytes()).hexdigest()
    (L/'integration-summary.txt').write_text('fbfreerdp SHA256: '+digest+'\n'+'\n'.join(checks)+'\n')
finally:
    server.terminate()
    try:
        server.wait(timeout=5)
    except subprocess.TimeoutExpired:
        server.kill()
        server.wait()
