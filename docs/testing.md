# Validation and acceptance

## Automated checks performed on the build computer

The tests use QEMU user-mode ARM926 emulation, the original rootfs's BusyBox
1.14.2 ash, and controlled command fixtures for physical hardware/network
actions. They do not boot the SPEAr600 kernel or simulate NAND/VGA hardware.

```sh
python3 NCOMPUTING_L300_SPEAR600/appliance/tests/test_appliance.py
python3 NCOMPUTING_L300_SPEAR600/appliance/tests/test_image.py
python3 NCOMPUTING_L300_SPEAR600/appliance/tests/test_ssh.py
python3 FreeRDP-old-master/l300/verify.py
python3 FreeRDP-old-master/l300/test-integration.py
```

- 11 shell fault tests: strict config parsing, injection rejection, unsafe-mode
  rejection, setup gating, cable/DHCP gating, repeated failure backoff, no
  credentials in process arguments, certificate/support states, mid-session
  cable loss/recovery, DHCP bound/deconfig, persistent DHCP with renewal,
  disabled maintenance, DHCP server recovery, and crash/logout restart.
- 5 image tests: metadata-preserving cpio round trip; safe boot entries and
  static client; only FWD rootfs size changed; original backup hashes unchanged;
  flash-plan geometry/provisioning guards.
- Original FreeRDP tests: 51 tests / 198 assertions, plus 13 frontend self-tests
  and ELF/static dependency checks.
- Local xrdp integration in appliance mode: pinned TLS 1.2 connection and
  rendered 1024x768 login screen, incorrect/missing pins rejected with code 66,
  clean exit, and a stalled connection terminated with code 124.
- Original Dropbear integration: disposable public key accepted, exact host
  key checked, password authentication not offered. No remote shell was run.
- The generated setup screen was rendered to a memory framebuffer and
  visually inspected. This is not physical VGA validation.

Shell tests shorten sleep through a fixture command; production scripts keep
their real delays. The scripts run under an unprivileged user namespace that
maps the test account to root, allowing real permission checks without host
root. Local server tests require localhost sockets and user namespaces. No
real network interface or persistent host service is modified.

Logs are under `l300-appliance-build/` and `l300-build/logs/`. Known legacy
compiler warnings and xrdp's optional capability notice remain; a successful
test does not certify all protocol paths in this 2011 source.

## Physical acceptance checklist

Perform phases in order: RAM boot/network → connection → retry/failure →
shell isolation → Windows login UX → maintenance → final deployment. Keep
serial diagnostics until these pass. Record firmware hashes, device ID/MAC,
server OS/build, listener certificate/policy, lease details, logs, and results.

| Required case | Procedure and expected result | Current evidence |
| --- | --- | --- |
| 1. Normal boot | RAM-boot candidate; hardware initialization finishes, display shows status, no Linux prompt | Structure/shell tests; board pending |
| 2. DHCP success | Boot with active DHCP; verify address, subnet, route, DNS and ongoing lease renewal | Hook/daemon fixtures; real DHCP pending |
| 3. No DHCP server | Disable DHCP; no RDP launch; restore DHCP; board eventually connects without typing IP settings | Fixture passed; board pending |
| 4. Cable absent at boot | Boot unplugged, then plug in; network status changes to connecting | Fixture passed; board pending |
| 5. Cable lost in session | Unplug after login; old remote screen disappears, no shell; reconnect and authenticate again | Process fixture passed; board pending |
| 6. Server unavailable | Stop RDS; check bounded connection timeout and increasing retry delay | Loopback timeout/retry fixtures passed |
| 7. Server reboot | Reboot Windows during a session; device returns to connection/login once available | Repeated-failure fixture; Windows pending |
| 8. Wrong username/password | Enter incorrect values on Windows screen; server permits retry or client reconnects; no BusyBox | Windows pending |
| 9. Successful login | Sign in to Windows, type, move/click mouse, use an application; record RAM/CPU/display quality | xrdp login screen only; Windows pending |
| 10. Logout | Sign out of Windows; next user receives a login screen, not the prior user's desktop | Exit-0 restart fixture; Windows pending |
| 11. RDP disconnect | Disconnect without signing out; client retries; reauthenticate to reconnect per server policy | Loopback clean exit and retry fixture |
| 12. Client crash | Authorized technician kills only fbfreerdp; init/supervisor remain and launch a fresh client | SIGKILL fixture passed |
| 13. L300 reboot | Reboot twice; baked config/pin/MAC persist, RAM logs/leases reset, DHCP reruns | Cpio/config inspection; board pending |
| 14. Multiple L300s | Build unique device IDs/MACs; boot at least three; verify distinct leases and simultaneous connections | Provisioning design; fleet pending |
| 15. Different users | Alice/Bob/Carol log in on different devices; verify distinct sessions via RDS administration | Windows pending |
| 16. Same user moves | Disconnect Alice on one board; sign in as Alice on another; verify policy reconnects Alice's session without exposing it to Bob | Windows pending |
| 17. Changed certificate | Replace test listener certificate; old pin blocks all RDP attempts; verify/export new pin and rebuild to restore | Local pin mismatch passed; server update pending |
| 18. Administrator recovery | Test serial RAM-boot recovery before flash; optionally verify authorized key works, unauthorized/password login fails, and Ctrl+Alt+F12 never opens a shell | Public-key auth test passed; physical recovery pending |

Do not treat Windows multi-user behavior as a client-side test result. It
depends on RD Session Host, licensing, account permissions, NLA/TLS policy,
single-session-per-user policy, and disconnected-session retention. Do not
mark hardware-only rows passed based solely on these emulation results.
