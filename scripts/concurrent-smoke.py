#!/usr/bin/env python3
"""A busy CEF profile must report an error without opening a standalone browser."""
import ctypes as C
import json
import os
from pathlib import Path
import platform
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
RUNTIME = Path(os.environ.get('WEBBROWSER_RUNTIME', ROOT/'plugins/webbrowser/runtime')).resolve()
class State(C.Structure):
    _fields_ = [('revision',C.c_uint64),('frame',C.c_uint64),('context_revision',C.c_uint64)] + [
        (name,C.c_uint32) for name in ['width','height','status','loading','can_back','can_forward']
    ] + [(name,C.c_int32) for name in ['caret_x','caret_y','caret_width','caret_height']] + [
        ('url',C.c_char*4096),('title',C.c_char*512),('error',C.c_char*1024),('cursor',C.c_char*16)]
bridge = C.CDLL(str(RUNTIME/('webbrowser-bridge.dll' if os.name=='nt' else 'webbrowser-bridge.so')))
bridge.wb_create.argtypes,bridge.wb_create.restype = [C.c_char_p],C.c_void_p
bridge.wb_poll.argtypes = [C.c_void_p,C.POINTER(State)]
bridge.wb_destroy.argtypes = [C.c_void_p]
assert bridge.wb_state_size()==C.sizeof(State)
executable = RUNTIME/('webbrowser-host.exe' if os.name=='nt' else
    'webbrowser-host.app/Contents/MacOS/webbrowser-host' if platform.system()=='Darwin' else 'webbrowser-host')

with tempfile.TemporaryDirectory(prefix='webbrowser-concurrent-') as directory:
    directory = Path(directory)
    profile = directory/'profile'
    hosts = []
    def launch(name):
        filename=directory/(name+'.map')
        handle=bridge.wb_create(os.fsencode(filename));assert handle
        log=(directory/(name+'.log')).open('wb')
        args=[str(executable),'--transport='+str(filename),'--profile='+str(profile),
            '--log-file='+str(directory/(name+'-cef.log')),'--width=320','--height=240',
            '--url=data:text/html,<title>'+name+'</title>Browser profile test']
        if os.environ.get('WB_TEST_NO_SANDBOX'):args.append('--no-sandbox')
        child=subprocess.Popen(args,stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=log)
        item={'process':child,'handle':handle,'log':log,'name':name};hosts.append(item)
        return item
    def wait(item,condition):
        state=State();deadline=time.monotonic()+20
        while time.monotonic()<deadline:
            bridge.wb_poll(item['handle'],C.byref(state))
            if condition(state):return state
            time.sleep(.025)
        raise AssertionError(str((item['name'],state.status,state.title,state.error)))
    def send(item,kind,**values):
        item['process'].stdin.write((json.dumps(dict(type=kind,id=1,**values))+'\n').encode())
        item['process'].stdin.flush()
    def stop(item):
        child=item['process']
        if child.poll() is None:
            child.stdin.close();child.wait(timeout=15)
        item['log'].close()
    try:
        first=launch('first');wait(first,lambda s:s.frame>0)
        second=launch('second')
        state=wait(second,lambda s:s.status==3)
        assert b'already in use' in state.error,state.error
        second['process'].wait(timeout=15)
        assert second['process'].returncode!=0
        assert not (directory/'profile-2').exists()
        print('PASS a busy profile is rejected without creating another profile',flush=True)
        send(first,'script',code="document.title='first still works'")
        wait(first,lambda s:s.title==b'first still works')
        print('PASS the existing editor browser remains usable',flush=True)
        send(first,'html',html='<title>Storage</title>',base_url='')
        send(first,'script',code="localStorage.setItem('profile-test','saved');document.title='stored'")
        wait(first,lambda s:s.title==b'stored');stop(first)
        again=launch('again');wait(again,lambda s:s.frame>0)
        send(again,'html',html='<title>Storage</title>',base_url='')
        send(again,'script',code="document.title='value:'+localStorage.getItem('profile-test')")
        wait(again,lambda s:s.title==b'value:saved');stop(again)
        print('PASS the same persistent profile is reusable after the first editor exits',flush=True)
    finally:
        for item in hosts:
            child=item['process']
            if child.poll() is None:
                child.stdin.close()
                try:child.wait(timeout=10)
                except subprocess.TimeoutExpired:child.kill();child.wait()
            item['log'].close();bridge.wb_destroy(item['handle'])
        time.sleep(.2)
