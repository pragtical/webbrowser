#!/usr/bin/env python3
import ctypes as C
import json, os, subprocess, tempfile, time
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent
RUNTIME = Path(os.environ.get('WEBBROWSER_RUNTIME', ROOT/'plugins/webbrowser/runtime')).resolve()
class State(C.Structure):
    _fields_ = [('revision',C.c_uint64),('frame',C.c_uint64),('context_revision',C.c_uint64)] + [(n,C.c_uint32) for n in ['width','height','status','loading','can_back','can_forward']] + [(n,C.c_int32) for n in ['caret_x','caret_y','caret_width','caret_height']] + [('url',C.c_char*4096),('title',C.c_char*512),('error',C.c_char*1024),('cursor',C.c_char*16)]
lib=C.CDLL(str(RUNTIME/('webbrowser-bridge.dll' if os.name=='nt' else 'webbrowser-bridge.so')))
lib.wb_create.argtypes=[C.c_char_p];lib.wb_create.restype=C.c_void_p
lib.wb_poll.argtypes=[C.c_void_p,C.POINTER(State)]
lib.wb_destroy.argtypes=[C.c_void_p]
lib.wb_lock_frame.argtypes=[C.c_void_p,C.c_uint64,C.POINTER(State)];lib.wb_lock_frame.restype=C.c_void_p
lib.wb_unlock.argtypes=[C.c_void_p]
lib.wb_lock_clipboard.argtypes=[C.c_void_p,C.c_uint64,C.POINTER(C.c_uint32),C.POINTER(C.c_uint64)]
lib.wb_lock_clipboard.restype=C.c_void_p
lib.wb_lock_context.argtypes=[C.c_void_p,C.c_uint64,C.POINTER(C.c_uint32),C.POINTER(C.c_uint64)]
lib.wb_lock_context.restype=C.c_void_p
assert lib.wb_abi_version()==5
assert lib.wb_state_size()==C.sizeof(State)
with tempfile.TemporaryDirectory(prefix='webbrowser-test-') as temp:
    temp=Path(temp)
    html=temp/'test.html'
    html.write_text('''<!doctype html><meta charset="utf-8"><style>body{margin:0;background:rgb(10,20,30)}input{width:300px;height:40px}button{height:50px}</style><input autofocus oninput="document.title=this.value"><button onclick="document.title='clicked'">Click me</button><select><option>One</option><option>Two</option></select><script>document.title='ready';let x=0;setInterval(()=>{document.body.style.backgroundColor=`rgb(${10+(++x%100)},20,30)`},100)</script>''')
    handles={i:lib.wb_create(os.fsencode(temp/f'{i}.map')) for i in [1,2,3]}
    assert all(handles.values())
    log=(temp/'stderr').open('wb')
    args=[str(RUNTIME/('webbrowser-host.exe' if os.name=='nt' else 'webbrowser-host.app/Contents/MacOS/webbrowser-host' if __import__('sys').platform=='darwin' else 'webbrowser-host')),f'--transport={temp}/1.map',f'--url={html.as_uri()}',f'--profile={temp}/profile','--width=640','--height=480','--fps=30']
    if os.environ.get('WB_TEST_NO_SANDBOX'): args+=['--no-sandbox']
    host=subprocess.Popen(args,stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=log)
    def send(kind,id=1,**data):
        host.stdin.write((json.dumps(dict(type=kind,id=id,**data))+'\n').encode());host.stdin.flush()
    def wait(predicate,id=1,timeout=15):
        start=time.monotonic();state=State()
        while time.monotonic()-start<timeout:
            lib.wb_poll(handles[id],C.byref(state))
            if predicate(state): return state
            if host.poll() is not None: raise AssertionError((temp/'stderr').read_text()[-6000:])
            time.sleep(.025)
        raise AssertionError(f'timeout id{id} state={state.status},size={state.width}x{state.height},frame={state.frame},title={state.title},err={state.error}\n'+(temp/'stderr').read_text()[-4000:])
    try:
        s=wait(lambda s:s.frame>2 and s.title==b'ready')
        assert s.cursor in [b'arrow',b'ibeam',b'hand',b'sizeh',b'sizev'],s.cursor
        frame=s.frame;wait(lambda s:s.frame>frame)
        print('PASS changing frames',flush=True)
        ptr=lib.wb_lock_frame(handles[1],0,C.byref(s));assert ptr
        pixel=C.string_at(ptr,4);lib.wb_unlock(handles[1]);assert pixel[1:]==bytes([20,30,255]),pixel
        print('PASS RGBA transfer',flush=True)
        send('text',text='Hello multiline 🌎',modifiers=0)
        wait(lambda s:s.title.decode()=='Hello multiline 🌎')
        print('PASS unicode text',flush=True)
        send('key', key=65, native=0, modifiers=4, up=False)
        send('key', key=65, native=0, modifiers=4, up=True)
        send('text', text='Replaced', modifiers=0)
        wait(lambda s:s.title==b'Replaced')
        print('PASS select-all shortcut',flush=True)
        send('composition', text='文', start=1, length=0)
        send('commit', text='文章')
        wait(lambda s:s.title.decode()=='Replaced文章')
        print('PASS IME composition and commit',flush=True)
        send('key',key=8,native=0,modifiers=0,up=False)
        send('text',text='\b',modifiers=0)
        send('key',key=8,native=0,modifiers=0,up=True)
        wait(lambda s:s.title.decode()=='Replaced文')
        print('PASS backspace deletes one character',flush=True)
        send('resize',width=1000,height=700,scale=1.25)
        wait(lambda s:s.width==1000 and s.height==700)
        print('PASS fractional DPI resize',flush=True)
        send('resize',width=1000,height=700,scale=1)
        send('script', code="setTimeout(()=>document.title=String(window.devicePixelRatio),100)")
        wait(lambda s:s.title==b'1')
        send('resize',width=1000,height=700,scale=1.25)
        send('script', code="setTimeout(()=>document.title=String(window.devicePixelRatio),100)")
        wait(lambda s:s.title.startswith(b'1.') and 1.2<float(s.title)<1.3)
        print('PASS integer and fractional scale values',flush=True)
        send('button',x=440,y=25,button=0,up=False,clicks=1)
        send('button',x=440,y=25,button=0,up=True,clicks=1)
        wait(lambda s:s.title==b'clicked')
        print('PASS mouse input',flush=True)
        send('create',id=2,transport=str(temp/'2.map'),url='data:text/html,<title>second</title>Second view',width=320,height=200,scale=1.0,fps=30)
        wait(lambda s:s.frame>0 and s.title==b'second',id=2)
        wait(lambda s:s.title==b'clicked')
        print('PASS independent tabs in shared host',flush=True)
        send('navigate',id=2,url='data:text/html,<title>third</title>Third page')
        wait(lambda s:s.title==b'third' and s.can_back,id=2)
        send('back',id=2);wait(lambda s:s.title==b'second' and s.can_forward,id=2)
        print('PASS navigation history',flush=True)
        send('html',id=2,html='<title>HTML mémoire</title><h1 id="heading">Memory</h1>',base_url='')
        send('script',id=2,code="document.title=document.getElementById('heading').textContent+' ready'")
        wait(lambda s:s.title==b'Memory ready',id=2)
        send('reload',id=2)
        wait(lambda s:s.title.decode()=='HTML mémoire',id=2)
        print('PASS in-memory HTML, queued JavaScript and reload',flush=True)
        css=temp/'style.css';css.write_text('body{background:rgb(77,88,99)}')
        send('html',id=2,html='<link rel="stylesheet" href="style.css"><title>Local assets</title>',base_url=(temp/'preview.html').as_uri())
        wait(lambda s:s.title==b'Local assets' and not s.loading,id=2)
        send('script',id=2,code="document.title=getComputedStyle(document.body).backgroundColor")
        wait(lambda s:s.title==b'rgb(77, 88, 99)',id=2)
        print('PASS relative local HTML resources',flush=True)
        send('html',id=2,html='<title>Canonical URL</title>',base_url='https://pragtical-html.invalid')
        wait(lambda s:s.title==b'Canonical URL',id=2)
        print('PASS canonical HTML base URL',flush=True)
        send('script',id=2,code="document.body.innerHTML='<pre id=selection></pre><textarea id=input></textarea>';document.getElementById('selection').textContent='first line\\nsecond Ω🌎';let r=document.createRange();r.selectNodeContents(document.getElementById('selection'));let s=window.getSelection();s.removeAllRanges();s.addRange(r);document.title='selected'")
        wait(lambda s:s.title==b'selected',id=2)
        sequence=C.c_uint64(0);size=C.c_uint32(0)
        def copy(expected,**options):
            old=sequence.value;send('copy',id=2,**options)
            deadline=time.monotonic()+10
            while time.monotonic()<deadline:
                pointer=lib.wb_lock_clipboard(handles[2],old,C.byref(size),C.byref(sequence))
                if pointer:
                    text=C.string_at(pointer,size.value).decode();lib.wb_unlock(handles[2]);assert text==expected,(text,expected)
                    return
                time.sleep(.025)
            raise AssertionError('No clipboard response')
        copy('first line\nsecond Ω🌎')
        send('script',id=2,code="let input=document.getElementById('input');input.value='textarea selection';input.focus();input.setSelectionRange(0,8);document.title='input selected'")
        wait(lambda s:s.title==b'input selected',id=2)
        copy('textarea')
        print('PASS multiline Unicode and input selection clipboard transfer',flush=True)
        send('html',id=2,html='''<title>Context test</title><style>body{margin:0}textarea{position:absolute;left:0;top:0;width:300px;height:60px}a{position:absolute;left:10px;top:100px}</style><textarea id="field" oninput="document.title=JSON.stringify(this.value)"></textarea><a id="target" href="https://example.com/context">Context link</a><iframe id="nested" style="position:absolute;left:0;top:150px;width:300px;height:100px" srcdoc="<textarea oninput='parent.document.title=JSON.stringify(this.value)'></textarea>"></iframe>''',base_url='')
        wait(lambda s:s.title==b'Context test' and not s.loading,id=2)
        send('script',id=2,code="let field=document.getElementById('field');field.value='alpha beta';field.focus();field.setSelectionRange(0,5);document.title='context selected'")
        wait(lambda s:s.title==b'context selected',id=2)
        context_revision=C.c_uint64(0);context_size=C.c_uint32(0)
        def context_at(x,y):
            old=context_revision.value
            send('button',id=2,x=x,y=y,button=2,up=False,clicks=1)
            send('button',id=2,x=x,y=y,button=2,up=True,clicks=1)
            deadline=time.monotonic()+10
            while time.monotonic()<deadline:
                pointer=lib.wb_lock_context(handles[2],old,C.byref(context_size),C.byref(context_revision))
                if pointer:
                    message=C.string_at(pointer,context_size.value);lib.wb_unlock(handles[2])
                    if message:return json.loads(message)
                    old=context_revision.value
                time.sleep(.025)
            raise AssertionError('No context menu snapshot')
        context=context_at(12,10)
        assert context['editable'] and context['copy'] and context['cut'],context
        copy('alpha',context=context['token'])
        send('context-action',id=2,token=context['token'],action='delete')
        wait(lambda s:s.title==b'" beta"',id=2)
        context=context_at(3,10)
        send('context-action',id=2,token=context['token'],action='paste',text='Ω\nnext')
        wait(lambda s:s.title.decode()==json.dumps('Ω\nnext beta',ensure_ascii=False),id=2)
        context=context_at(20,110)
        assert context['link']=='https://example.com/context' and not context['editable'],context
        print('PASS contextual targets, copy, cut acknowledgement and multiline paste',flush=True)
        send('resize',id=2,width=500,height=400,scale=1)
        wait(lambda s:s.height==400,id=2)
        send('script',id=2,code="let nested=document.getElementById('nested').contentDocument.querySelector('textarea');nested.value='iframe selection';nested.focus();nested.setSelectionRange(0,6);document.title='iframe selected'")
        wait(lambda s:s.title==b'iframe selected',id=2)
        context=context_at(15,165)
        assert context['editable'] and context['copy'],context
        copy('iframe',context=context['token'])
        send('context-action',id=2,token=context['token'],action='paste',text='frame Ω')
        wait(lambda s:s.title.decode()==json.dumps('frame Ω selection',ensure_ascii=False),id=2)
        print('PASS context actions target the original iframe',flush=True)
        send('devtools',id=2,child_id=3,transport=str(temp/'3.map'),width=500,height=400,scale=1.0,fps=30,x=20,y=110)
        wait(lambda s:s.status==1 and s.frame>0 and b'/devtools/devtools_app.html?' in s.url and not s.loading,id=3)
        send('script',id=3,code="""(async()=>{const SDK=await import(new URL('./core/sdk/sdk.js',location.href).href);for(let i=0;i<100;i++){const target=SDK.TargetManager.TargetManager.instance().primaryPageTarget();if(target){await target.runtimeAgent().invoke_evaluate({expression:\"document.title='Inspector edited page'\"});return;}await new Promise(resolve=>setTimeout(resolve,50));}})()""")
        wait(lambda s:s.title==b'Inspector edited page',id=2)
        print('PASS DevTools frontend controls its inspected page',flush=True)
        send('script',id=3,code="document.title='Inspector frontend ready'")
        wait(lambda s:s.title==b'Inspector frontend ready',id=3)
        send('close',id=3)
        wait(lambda s:s.status==2,id=3)
        send('script',id=2,code="document.title='page survives inspector close'")
        wait(lambda s:s.title==b'page survives inspector close',id=2)
        send('devtools',id=2,child_id=3,transport=str(temp/'3.map'),width=500,height=400,scale=1.25,fps=30)
        wait(lambda s:s.status==1 and s.frame>0 and b'/devtools/devtools_app.html?' in s.url and not s.loading,id=3)
        send('script',id=3,code="document.title='scale:'+devicePixelRatio")
        scaled=wait(lambda s:s.title.startswith(b'scale:'),id=3)
        assert 1.2<float(scaled.title[6:])<1.3,scaled.title
        print('PASS off-screen DevTools, independent document and close/reopen',flush=True)
        send('visible',value=False);time.sleep(.5);s=wait(lambda s:True);frame=s.frame;time.sleep(.5)
        assert wait(lambda s:True).frame==frame
        print('PASS hidden tab stops painting',flush=True)
        send('close',id=1);wait(lambda s:s.status==2)
        send('script',id=2,code="document.title='still alive'")
        wait(lambda s:s.title==b'still alive',id=2)
        send('quit');host.wait(timeout=15);assert host.returncode==0,host.returncode
        print('PASS clean shutdown',flush=True)
    finally:
        if host.poll() is None:
            host.stdin.close()
            try: host.wait(timeout=10)
            except subprocess.TimeoutExpired: host.kill();host.wait()
        time.sleep(.2)
        log.close()
        for handle in handles.values():lib.wb_destroy(handle)
