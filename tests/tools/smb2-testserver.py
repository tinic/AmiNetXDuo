#!/usr/bin/env python3
#
# An SMB2 server for install/test/run-smbmount.sh to mount, on a machine that
# is not the one running the emulator.
#
#     tests/tools/smb2-testserver.py [PORT]        default 4445
#
# SPDX-License-Identifier: MIT
import binascii, os, sys, logging, struct
from impacket import smbserver
from impacket import smb3structs as smb2
from impacket.smbserver import SimpleSMBServer
from impacket.ntlm import compute_nthash
from impacket.nt_errors import STATUS_SUCCESS

share = os.path.expanduser("~/smbshare")
os.makedirs(os.path.join(share, "Drawer"), exist_ok=True)
with open(os.path.join(share, "hello.txt"), "w") as f:
    f.write("Hello from an SMB share.\n")
with open(os.path.join(share, "Drawer", "inner.txt"), "w") as f:
    f.write("inner\n")


def query_info(connId, smbServer, recvPacket):
    cmds, pkts, err = smbserver.SMB2Commands.smb2QueryInfo(connId, smbServer,
                                                          recvPacket)
    try:
        q = smb2.SMB2QueryInfo(recvPacket['Data'])
        if (q['InfoType'] == smb2.SMB2_0_INFO_FILESYSTEM
                and err == STATUS_SUCCESS):
            st = os.statvfs(share)
            unit = 8
            sector = 512
            per = unit * sector
            total = st.f_blocks * st.f_frsize // per
            avail = st.f_bavail * st.f_frsize // per
            if q['FileInfoClass'] == 7:          # FileFsFullSizeInformation
                buf = struct.pack('<qqqII', total, avail, avail, unit, sector)
            elif q['FileInfoClass'] == 3:        # FileFsSizeInformation
                buf = struct.pack('<qqII', total, avail, unit, sector)
            else:
                buf = None
            if buf is not None:
                cmds[0]['Buffer'] = buf
                cmds[0]['OutputBufferLength'] = len(buf)
    except Exception as e:
        print("query_info hook failed:", e, flush=True)
    return cmds, pkts, err


_orig_error_getData = smb2.SMB2Error.getData


def _error_getData(self, *a, **k):
    d = _orig_error_getData(self, *a, **k)
    if len(d) < 9:
        d += b'\x00' * (9 - len(d))
    return d


smb2.SMB2Error.getData = _error_getData

logging.basicConfig(level=logging.INFO)
port = int(sys.argv[1]) if len(sys.argv) > 1 else 4445
s = SimpleSMBServer(listenAddress="0.0.0.0", listenPort=port)
s.addShare("RETRO", share, "test share")
s.setSMB2Support(True)
s.addCredential("amiga", 0, "", binascii.hexlify(compute_nthash("bantha")).decode())
srv = s._SimpleSMBServer__server
srv.hookSmb2Command(smb2.SMB2_QUERY_INFO, query_info)

NOT_REALLY_ERRORS = (0x00000000, 0xC0000016, 0x80000005, 0x00000103)


def error_body(orig):
    def wrapper(connId, smbServer, recvPacket):
        cmds, pkts, err = orig(connId, smbServer, recvPacket)
        if err not in NOT_REALLY_ERRORS and cmds:
            cmds = [smb2.SMB2Error()]
        return cmds, pkts, err
    return wrapper


for _cmd in (smb2.SMB2_CREATE, smb2.SMB2_CLOSE, smb2.SMB2_QUERY_INFO,
             smb2.SMB2_QUERY_DIRECTORY, smb2.SMB2_READ, smb2.SMB2_WRITE,
             smb2.SMB2_SET_INFO, smb2.SMB2_FLUSH, smb2.SMB2_IOCTL,
             smb2.SMB2_TREE_CONNECT):
    _orig = srv._SMBSERVER__smb2Commands.get(_cmd)
    if _orig is not None:
        srv.hookSmb2Command(_cmd, error_body(_orig))
s.setLogFile("")
print("serving %s on port %d as amiga/bantha" % (share, port), flush=True)
s.start()
