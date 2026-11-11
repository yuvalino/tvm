# Generates the mockup HTML pages; render.sh turns them into PNGs.
import pathlib

HERE = pathlib.Path(__file__).parent

HEAD = """<!doctype html><html><head><meta charset="utf-8">
<link href="https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@400;500;700&display=swap" rel="stylesheet">
<link rel="stylesheet" href="base.css"><style>{css}</style></head><body>"""

STATUS = """<div class="notch"></div>
<div class="status"><span>9:41</span><span class="sr">
<svg width="18" height="12" viewBox="0 0 18 12"><rect x="0" y="8" width="3" height="4" rx="1" fill="#f2ece0"/><rect x="5" y="5" width="3" height="7" rx="1" fill="#f2ece0"/><rect x="10" y="2" width="3" height="10" rx="1" fill="#f2ece0"/><rect x="15" y="0" width="3" height="12" rx="1" fill="#f2ece0"/></svg>
<svg width="26" height="12" viewBox="0 0 26 12"><rect x=".5" y=".5" width="22" height="11" rx="3" fill="none" stroke="#f2ece0" opacity=".5"/><rect x="2" y="2" width="16" height="8" rx="2" fill="#f2ece0"/><rect x="24" y="4" width="2" height="4" rx="1" fill="#f2ece0" opacity=".5"/></svg></span></div>"""

def svg(d, cls=""):
    return f'<svg class="{cls}" viewBox="0 0 24 24"><path d="{d}"/></svg>'

# The site's neofetch apple: steel shackle hanging open off the left, body
# striped green, yellow, red, magenta, blue.
_uid = [0]
def apple(size=20):
    _uid[0] += 1
    g = f"ap{_uid[0]}"
    return f"""<svg width="{size}" height="{size}" viewBox="0 0 24 24" style="flex:0 0 auto">
<defs><linearGradient id="{g}" x1="0" y1="0" x2="0" y2="1">
<stop offset="0" stop-color="#b2c364"/><stop offset=".22" stop-color="#b2c364"/>
<stop offset=".22" stop-color="#e0b352"/><stop offset=".40" stop-color="#e0b352"/>
<stop offset=".40" stop-color="#ea6962"/><stop offset=".62" stop-color="#ea6962"/>
<stop offset=".62" stop-color="#d3869b"/><stop offset=".80" stop-color="#d3869b"/>
<stop offset=".80" stop-color="#83b6ac"/><stop offset="1" stop-color="#83b6ac"/></linearGradient></defs>
<path d="M6.3 8.6V5.2a3.4 3.4 0 0 1 6.8 0V6.4" fill="none" stroke="#c3bcae" stroke-width="2" stroke-linecap="round"/>
<path fill="url(#{g})" d="M17.2 14.1c0-2.5 2-3.7 2.1-3.8-1.2-1.7-3-1.9-3.6-1.9-1.5-.2-3 .9-3.8.9-.8 0-2-.9-3.3-.8-1.7 0-3.2 1-4.1 2.5-1.8 3.1-.5 7.6 1.3 10.1.8 1.2 1.8 2.6 3.1 2.5 1.2 0 1.7-.8 3.2-.8s1.9.8 3.2.8c1.3 0 2.2-1.2 3-2.5.9-1.4 1.3-2.8 1.3-2.9 0 0-2.4-1-2.4-4.1z"/></svg>"""

# Shell tabs: a ">_" prompt in a rounded box.
SHELL = ('<svg width="18" height="18" viewBox="0 0 24 24" style="flex:0 0 auto;fill:none;stroke:currentColor;'
         'stroke-width:2;stroke-linecap:round;stroke-linejoin:round"><rect x="2.5" y="3.5" width="19" height="17" rx="4"/>'
         '<path d="M7 9.5l3 2.5-3 2.5M12.5 15h4.5"/></svg>')
PLUS = svg("M12 5v14M5 12h14")
KB_UP = ('<svg viewBox="0 0 24 24"><rect x="2.5" y="5" width="19" height="11" rx="2"/>'
         '<path d="M6 8.5h1M9.5 8.5h1M13 8.5h1M16.5 8.5h1M8 12.5h8"/><path d="M9 21l3-2.5 3 2.5"/></svg>')
KB_DOWN = ('<svg viewBox="0 0 24 24"><rect x="2.5" y="3" width="19" height="11" rx="2"/>'
           '<path d="M6 6.5h1M9.5 6.5h1M13 6.5h1M16.5 6.5h1M8 10.5h8"/><path d="M9 18.5l3 2.5 3-2.5"/></svg>')
CHEV = svg("M9 6l6 6-6 6", "ch")

COMMON = """
.term{background:var(--bg);color:#e5e5e5;font:12.5px/1.45 Menlo,monospace;padding:4px 8px;overflow:hidden}
.term p{white-space:pre}
.cur{background:#e5e5e5;color:var(--bg)}
.bl{color:#2472c8;font-weight:bold}
.bar{position:absolute;left:0;right:0;bottom:0;background:var(--bar);border-top:1px solid var(--line)}
.bar.closed{height:86px}
.brow{display:flex;gap:6px;padding:8px 8px 0;align-items:center}
.tb{height:40px;display:flex;align-items:center;gap:6px;padding:0 9px;border-radius:11px;color:var(--dim);font:500 13px var(--mono);border:1px solid transparent;white-space:nowrap;flex:0 0 auto}
.tb svg{width:18px;height:18px}
.tb.on{background:var(--card);border-color:var(--line);color:var(--bright)}
.tb.tvm{color:var(--fg);font-weight:700}
.tb.tvm svg{width:24px;height:24px}
.tb.tvm.on{color:var(--bright)}
.tb.tvm.mini{padding:0 10px}
.tb .dot{width:6px;height:6px;border-radius:50%;background:var(--green)}
.pin{display:flex;align-items:center;gap:6px;flex:0 0 auto}
.pin.shadow{padding-right:6px;border-right:1px solid var(--line)}
.strip{display:flex;gap:6px;overflow:hidden;flex:1;-webkit-mask-image:linear-gradient(90deg,transparent 0,#000 14px,#000 90%,transparent)}
.strip .lead{margin-left:-34px}
.btn{width:40px;height:40px;border-radius:11px;display:grid;place-items:center;flex:0 0 auto;border:1px solid var(--line);background:var(--card);color:var(--fg)}
.btn svg{width:20px;height:20px;stroke:currentColor;fill:none;stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round}
.btn.add{color:var(--green);border-style:dashed;background:transparent}
.btn.kbd{color:var(--yellow)}
.keys{display:flex;gap:6px;overflow:hidden;flex:1;-webkit-mask-image:linear-gradient(90deg,#000 85%,transparent)}
.key{min-width:40px;height:36px;padding:0 9px;border-radius:9px;background:var(--card);border:1px solid var(--line);display:grid;place-items:center;font:500 13px var(--mono);color:var(--fg);flex:0 0 auto}
.key.mod{color:var(--yellow)}.key.on{background:var(--yellow);color:#1f1f1e;border-color:var(--yellow)}
.key svg{width:14px;height:14px;stroke:currentColor;fill:none;stroke-width:2.2;stroke-linecap:round;stroke-linejoin:round}
.bar.open{bottom:291px;padding-bottom:8px}
.kb{position:absolute;left:0;right:0;bottom:0;height:291px;background:#2c2c2e;padding:8px 3px 0}
.krow{display:flex;justify-content:center;gap:6px;margin-bottom:11px}
.k{width:33px;height:43px;border-radius:5px;background:#6b6b6e;color:#fff;display:grid;place-items:center;font:400 22px var(--sans);box-shadow:0 1px 0 #000}
.k.f{background:#464649;font-size:15px}
"""

def tab(name, on=False, dot=False):
    return (f'<div class="tb{" on" if on else ""}">{SHELL}{name}'
            f'{"<span class=dot></span>" if dot else ""}</div>')

def tvm_tab(on=False, mini=False):
    return (f'<div class="tb tvm{" on" if on else ""}{" mini" if mini else ""}">{apple()}'
            f'{"" if mini else "TVM"}</div>')

def tab_row(active, scrolled=False, names=("bash", "bash 2", "bash 3", "bash 4")):
    """The TVM tab is pinned at the left. With the strip at its start it reads
    "TVM"; scrolled away it shrinks to the apple and stays put."""
    tabs = "".join(tab(n, on=active == i + 1, dot=i == 0 and active != 1)
                   for i, n in enumerate(names))
    return (f'<div class="pin{" shadow" if scrolled else ""}">{tvm_tab(active == 0, mini=scrolled)}</div>'
            f'<div class="strip">{"<div class=lead></div>" if scrolled else ""}{tabs}</div>'
            f'<div class="btn add">{PLUS}</div>')

def bar_closed(active, scrolled=False, kbd=True, **kw):
    btn = f'<div class="btn kbd">{KB_UP}</div>' if kbd else ""
    return (f'<div class="bar closed"><div class="brow">{tab_row(active, scrolled, **kw)}{btn}</div></div>'
            '<div class="home"></div>')

def bar_open(active, scrolled=False):
    keys = ('<div class="key">esc</div><div class="key on">ctrl</div><div class="key mod">alt</div>'
            '<div class="key">tab</div>'
            + "".join(f'<div class="key">{svg(d)}</div>' for d in
                      ["M15 6l-6 6 6 6", "M6 15l6-6 6 6", "M6 9l6 6 6-6", "M9 6l6 6-6 6"])
            + '<div class="key">|</div><div class="key">~</div><div class="key">/</div>')
    k = lambda s, w=None, f=False: f'<div class="k{" f" if f else ""}"{f" style=width:{w}px" if w else ""}>{s}</div>'
    rows = ["".join(k(c) for c in "qwertyuiop"), "".join(k(c) for c in "asdfghjkl"),
            k("⇧", 43, True) + '<div style="width:6px"></div>' + "".join(k(c) for c in "zxcvbnm")
            + '<div style="width:6px"></div>' + k("⌫", 43, True),
            k("123", 88, True) + '<div class="k" style="width:186px;font-size:15px">space</div>' + k("return", 88, True)]
    return (f'<div class="bar open"><div class="brow">{tab_row(active, scrolled)}</div>'
            f'<div class="brow"><div class="keys">{keys}</div><div class="btn kbd">{KB_DOWN}</div></div></div>'
            '<div class="kb">' + "".join(f'<div class="krow">{r}</div>' for r in rows) + "</div>")

HOME_CSS = COMMON + """
.hdr{display:flex;align-items:center;gap:12px;padding:6px 16px 12px}
.hdr img{width:46px;height:46px;border-radius:11px;image-rendering:pixelated}
.hdr .t{font:700 20px var(--mono);color:var(--bright)}
.hdr .s{font:400 12px var(--mono);color:var(--dim);margin-top:2px}
.pill{margin-left:auto;display:inline-flex;align-items:center;gap:7px;padding:6px 11px;border-radius:999px;background:var(--card);font:500 12px var(--mono);color:var(--fg);border:1px solid var(--line)}
.pdot{width:7px;height:7px;border-radius:50%;background:var(--green);box-shadow:0 0 6px var(--green)}
.seg{display:flex;margin:0 16px 12px;background:var(--card);border:1px solid var(--line);border-radius:10px;padding:3px}
.seg div{flex:1;text-align:center;padding:7px 0;border-radius:8px;font:500 13px var(--mono);color:var(--dim)}
.seg .on{background:var(--hl);color:var(--bright);box-shadow:0 1px 3px rgba(0,0,0,.4)}
.card{margin:0 16px 10px;padding:12px;border:1px solid var(--line);border-radius:12px;background:var(--card)}
.card .t{color:var(--dim);font:600 11px var(--sans);letter-spacing:.06em;text-transform:uppercase;margin-bottom:6px}
.card .cmd{color:var(--bright);font:500 13px var(--mono);display:flex;justify-content:space-between}
.card .cp{font:600 12px var(--sans);color:var(--green)}
.log{padding:2px 16px 0;font:12px/1.55 var(--mono);overflow:hidden;height:470px;position:relative}
.log p{white-space:pre-wrap;overflow-wrap:anywhere;padding-left:2ch;text-indent:-2ch}
.lk{color:var(--yellow);font-weight:700}.i{color:var(--green);font-weight:700}.w{color:var(--yellow);font-weight:700}.d{color:var(--dim)}.b{color:var(--bright)}.c{color:var(--blue)}
.fab{position:absolute;right:14px;bottom:98px;display:flex;gap:8px}
.fb{height:36px;padding:0 12px;border-radius:10px;display:flex;align-items:center;gap:6px;font:500 12px var(--sans);border:1px solid var(--line);color:var(--fg);background:var(--card);box-shadow:0 6px 18px rgba(0,0,0,.4)}
.fb svg{width:15px;height:15px;stroke:currentColor;fill:none;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
.fb.on{background:var(--green);color:#1f1f1e;border-color:var(--green)}
.sh{font:700 12px var(--mono);color:var(--yellow);padding:10px 30px 8px;letter-spacing:.04em}
.grp{margin:0 16px;background:var(--card);border:1px solid var(--line);border-radius:14px;overflow:hidden}
.r{display:flex;align-items:center;padding:0 14px;height:48px;border-bottom:1px solid var(--line);font:400 16px var(--sans);color:var(--bright)}
.r:last-child{border-bottom:0}
.r .v{margin-left:auto;color:var(--dim);font:400 15px var(--mono)}
.r .ch{width:16px;height:16px;margin-left:8px;stroke:var(--dimmer);fill:none;stroke-width:2.4;stroke-linecap:round;stroke-linejoin:round}
.ic{width:30px;height:30px;border-radius:8px;display:grid;place-items:center;margin-right:12px}
.ic svg{width:17px;height:17px;stroke:#1f1f1e;fill:none;stroke-width:2.2;stroke-linecap:round;stroke-linejoin:round}
.sw{margin-left:auto;width:51px;height:31px;border-radius:16px;background:var(--green);position:relative}
.sw::after{content:"";position:absolute;right:2px;top:2px;width:27px;height:27px;border-radius:50%;background:#f2ece0}
.ft{font:400 12px var(--mono);color:var(--dimmer);padding:8px 30px 0}
.blocks{display:flex;justify-content:center;margin:20px 0 6px}.blocks i{width:22px;height:12px}
.by{text-align:center;font:400 12px var(--mono);color:var(--dimmer)}
"""

HEADER = """<div class="hdr"><img src="logo.png"><div><div class="t">TVM</div>
<div class="s">pan1c · 192.168.1.14</div></div>
<span class="pill"><span class="pdot"></span>ssh :2222</span></div>"""

def seg(which):
    return (f'<div class="seg"><div class="{"on" if which == "log" else ""}">log</div>'
            f'<div class="{"on" if which == "settings" else ""}">settings</div></div>')

LOG = """<div class="card"><div class="t">Connect from another machine</div><div class="cmd"><span>ssh -p 2222 mobile@pan1c.local</span><span class="cp">Copy</span></div></div>
<div class="log">
<p><span class="i">[INFO]</span> App started</p>
<p><span class="lk">device</span>   iPhone13,2_18.1.1_22B91</p>
<p><span class="lk">pid</span>      4821</p>
<p><span class="lk">uid</span>      501</p>
<p><span class="lk">username</span> mobile</p>
<p><span class="lk">hostname</span> pan1c.local</p>
<p><span class="lk">ssh port</span> 2222</p>
<p><span class="i">[INFO]</span> Starting tvm</p>
<p><span class="d">tvm_app_start:</span> booting, exec dropbear</p>
<p class="b">TVM initialized</p>
<p><span class="d">[4821] Oct 19 21:02:11</span> Not backgrounding</p>
<p><span class="d">[4830] Oct 19 21:02:40</span> Child connection from <span class="c">127.0.0.1</span>:51022</p>
<p><span class="d">[4830] Oct 19 21:02:40</span> Password auth succeeded for 'mobile' from <span class="c">127.0.0.1</span>:51022</p>
<p><span class="d">[4836] Oct 19 21:03:05</span> Child connection from <span class="c">192.168.1.20</span>:53412</p>
<p><span class="d">[4836] Oct 19 21:03:07</span> <span class="w">Bad password attempt</span> for 'mobile' from <span class="c">192.168.1.20</span>:53412</p>
<p><span class="d">[4836] Oct 19 21:03:09</span> Password auth succeeded for 'mobile' from <span class="c">192.168.1.20</span>:53412</p>
</div>
<div class="fab">
<div class="fb">""" + svg("M9 9h11v11H9zM5 15V5a2 2 0 0 1 2-2h8") + """Copy</div>
<div class="fb on">""" + svg("M12 5v14M6 13l6 6 6-6") + """Follow</div></div>"""

def row(icon_bg, icon, label, value=None, chev=True, switch=False):
    ic = f'<div class="ic" style="background:var(--{icon_bg})">{svg(icon)}</div>' if icon else ""
    v = f'<span class="v">{value}</span>' if value else ""
    tail = '<div class="sw"></div>' if switch else (CHEV if chev else "")
    return f'<div class="r">{ic}{label}{v}{tail}</div>'

FONT = "M4 7V4h16v3M9 20h6M12 4v16"
SETTINGS = (
    '<div class="sh">TERMINAL</div><div class="grp">'
    + row("blue", FONT, "Font Size", "13")
    + row("magenta", "M5 7l5 5-5 5M12 17h7", "Key Bar", "esc ctrl alt…")
    + '</div><div class="sh">LOG</div><div class="grp">'
    + row("blue", FONT, "Font Size", "12")
    + row("green", "M12 5v14M6 13l6 6 6-6", "Follow Output", switch=True)
    + '</div><div class="sh">SSH SERVER</div><div class="grp">'
    + row("yellow", "M3 12h18M12 3a14 14 0 0 1 0 18M12 3a14 14 0 0 0 0 18M12 3a9 9 0 1 0 0 18a9 9 0 1 0 0-18", "Port", "2222")
    + row("red", "M5 11h14v10H5zM8 11V7a4 4 0 0 1 8 0v4", "Password", "••••••")
    + '</div><div class="ft">Changes apply on next launch.</div>'
    + '<div class="sh">ABOUT</div><div class="grp">'
    + row(None, None, "Version", "1.0.0 (10)", chev=False)
    + '</div><div class="blocks">'
    + "".join(f'<i style="background:{c}"></i>' for c in
              ["#4a443c", "var(--red)", "var(--green)", "var(--yellow)",
               "var(--blue)", "var(--magenta)", "var(--cyan)", "#b5ac9c"])
    + '</div><div class="by">tvm by yuvalino</div>')

SHELL_LINES = """<p>$ uname -mnrs</p>
<p>Darwin pan1c 24.1.0 iPhone13,2</p>
<p>$ ls</p>
<p><span class="bl">Library</span>  <span class="bl">projects</span>  <span class="bl">tmp</span>  notes.md</p>
<p>$ python3</p>
<p>Python 3.15.0a0 (main) [Clang 19.1.7] on darwin</p>
<p>Type "help", "copyright", "credits" or "license"</p>
<p>for more information.</p>
<p>&gt;&gt;&gt; import os; os.uname().machine</p>
<p>'iPhone13,2'</p>
<p>&gt;&gt;&gt; exit()</p>
<p>$ git clone https://github.com/yuvalino/tvm</p>
<p>Cloning into 'tvm'...</p>
<p>remote: Enumerating objects: 4812, done.</p>
<p>Receiving objects: 100% (4812/4812), 9.1 MiB | 6</p>
<p>MiB/s, done.</p>
<p>Resolving deltas: 100% (2911/2911), done.</p>
<p>$ cd tvm &amp;&amp; git log --oneline -3</p>
<p>5644a1e bun: run compiled Bun programs (Claude C</p>
<p>ode 2.1.302) on the signed Bun</p>
<p>846ca25 llvm: an SDK of Apple's open-source relea</p>
<p>ses in the clang bundle</p>
<p>84a8b5b node: ship Node, Claude Code (2.1.112) an</p>
<p>d opencode runnable on iOS</p>
<p>$ <span class="cur"> </span></p>"""

OTHER_SHELL = """<p>$ top -o cpu</p>
<p>Processes: 14 total, 2 running</p>
<p>  PID COMMAND    %CPU  MEM</p>
<p> 4902 python3    41.2  38M</p>
<p> 4871 node       12.0  96M</p>
<p> 4830 dropbear    0.4 2.1M</p>
<p> 4831 bash        0.0 1.9M</p>"""

MENU_CSS = """
.dim{position:absolute;inset:0;background:rgba(0,0,0,.5)}
.lift{position:absolute}
.menu{position:absolute;left:96px;bottom:96px;width:230px;background:var(--card);border:1px solid var(--line);border-radius:14px;overflow:hidden;box-shadow:0 20px 50px rgba(0,0,0,.6)}
.mi{display:flex;justify-content:space-between;align-items:center;padding:13px 16px;font:400 16px var(--sans);color:var(--bright);border-bottom:1px solid var(--line)}
.mi:last-child{border-bottom:0}
.mi svg{width:18px;height:18px;stroke:currentColor;fill:none;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
.mi.red{color:var(--red)}
.sep{height:7px;background:var(--bg)}
.banner{position:absolute;left:12px;right:12px;bottom:100px;background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px;display:flex;align-items:center;gap:12px}
.banner .t{flex:1;font:500 14px var(--sans);color:var(--bright)}.banner .s{font:400 12px var(--mono);color:var(--dim);margin-top:3px}
.rbtn{height:36px;padding:0 14px;border-radius:10px;background:var(--green);color:#1f1f1e;font:600 14px var(--sans);display:grid;place-items:center}
"""

SWIPE_CSS = COMMON + """
.pages{position:absolute;top:47px;left:0;right:0;bottom:87px;overflow:hidden}
.pg{position:absolute;top:0;bottom:0;width:390px}
.pg.a{left:-150px}.pg.b{left:240px;box-shadow:-12px 0 30px rgba(0,0,0,.45)}
.pg .term{height:100%}
.hint{position:absolute;top:470px;left:50%;transform:translateX(-50%);display:flex;gap:8px;align-items:center;font:500 12px var(--mono);color:var(--dim);background:var(--card);border:1px solid var(--line);border-radius:999px;padding:6px 12px;white-space:nowrap}
.hint svg{width:14px;height:14px;stroke:currentColor;fill:none;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
"""

def page(name, css, body):
    (HERE / f"{name}.html").write_text(HEAD.format(css=css) + body + "</body></html>")

page("1-tvm-log", HOME_CSS, STATUS + HEADER + seg("log") + LOG + bar_closed(0, kbd=False))
page("2-tvm-settings", HOME_CSS, STATUS + HEADER + seg("settings") + SETTINGS + bar_closed(0, kbd=False))
page("3-shell-closed", COMMON, STATUS + f'<div class="term">{SHELL_LINES}</div>' + bar_closed(2))
page("4-shell-open", COMMON,
     STATUS + '<div class="term" style="height:408px;display:flex;flex-direction:column;justify-content:flex-end">'
     + SHELL_LINES + "</div>" + bar_open(2))
page("5-scrolled", COMMON,
     STATUS + f'<div class="term">{OTHER_SHELL}<p>$ <span class="cur"> </span></p></div>'
     + bar_closed(2, scrolled=True, names=("bash 3", "bash 4", "bash 5", "bash 6")))
page("6-swipe", SWIPE_CSS,
     STATUS + '<div class="pages">'
     f'<div class="pg a"><div class="term">{SHELL_LINES}</div></div>'
     f'<div class="pg b"><div class="term">{OTHER_SHELL}<p>$ <span class="cur"> </span></p></div></div>'
     '</div>'
     + bar_closed(2)
     + f'<div class="hint">{svg("M15 6l-6 6 6 6")}swipe: bash 2 → bash 3</div>')
page("7-tab-menu", COMMON + MENU_CSS,
     STATUS + '<div class="term"><p>$ exit</p><p>logout</p></div>'
     + '<div class="banner"><div><div class="t">Session ended</div><div class="s">exit 0 · connection closed</div></div><div class="rbtn">Reconnect</div></div>'
     + bar_closed(2) + '<div class="dim"></div>'
     + f'<div class="lift" id="lift">{tab("bash 2", on=True)}</div>'
     + '<div class="menu">'
     + f'<div class="mi">Rename{svg("M12 20h9M16.5 3.5a2.1 2.1 0 0 1 3 3L7 19l-4 1 1-4z")}</div>'
     + f'<div class="mi">New shell{SHELL}</div>'
     + f'<div class="mi">Reconnect{svg("M21 12a9 9 0 1 1-3-6.7L21 8M21 3v5h-5")}</div>'
     + '<div class="sep"></div>'
     + f'<div class="mi">Close others{svg("M4 4h16v16H4zM9 9l6 6M15 9l-6 6")}</div>'
     + f'<div class="mi red">Close tab{svg("M6 6l12 12M18 6L6 18")}</div></div>'
     # The lifted copy sits exactly over the real active tab.
     + """<script>const t=document.querySelector('.bar .tb.on').getBoundingClientRect();
const l=document.getElementById('lift');l.style.left=t.left+'px';l.style.top=t.top+'px';
l.firstChild.style.boxShadow='0 8px 24px rgba(0,0,0,.5)';</script>""")
