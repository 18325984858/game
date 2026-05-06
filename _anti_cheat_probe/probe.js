// Anti-cheat detection probe for Tencent DFM/PUBGMHD
// Target libs: libtersafe.so, libtprt.so
// Strategy: hook libc surface; filter by return address (must lie inside one of the target libs)
//           dump first hit per (function, source-pc) to keep noise down.

(function () {
    'use strict';

    const TARGET_LIBS = ['libtersafe.so', 'libtprt.so'];
    const HIT_CAP_PER_KEY = 3;   // max prints per (fn, source) tuple
    const TOTAL_CAP = 4000;
    const SECONDS_TO_RUN = 25;

    // Resolve target ranges once
    const ranges = [];
    function refreshRanges() {
        ranges.length = 0;
        for (const m of Process.enumerateModules()) {
            for (const want of TARGET_LIBS) {
                if (m.name === want || m.path.endsWith('/' + want)) {
                    ranges.push({ name: m.name, lo: m.base, hi: m.base.add(m.size) });
                }
            }
        }
    }
    refreshRanges();

    if (ranges.length === 0) {
        // libtprt may load later; install a small one-shot watcher
        const origDlopen = Module.findExportByName(null, 'android_dlopen_ext') || Module.findExportByName(null, 'dlopen');
        if (origDlopen) {
            Interceptor.attach(origDlopen, {
                onLeave: function () { try { refreshRanges(); } catch (e) {} }
            });
        }
    }

    function inTarget(addr) {
        for (const r of ranges) {
            if (addr.compare(r.lo) >= 0 && addr.compare(r.hi) < 0) return r;
        }
        return null;
    }

    let total = 0;
    const seen = new Map();      // key -> count

    function pushHit(fn, args, retval, srcRange, srcPc) {
        if (total >= TOTAL_CAP) return;
        const offset = srcPc.sub(srcRange.lo);
        const key = fn + '|' + srcRange.name + '+0x' + offset.toString(16);
        const n = (seen.get(key) || 0) + 1;
        if (n > HIT_CAP_PER_KEY) { seen.set(key, n); return; }
        seen.set(key, n);
        total += 1;
        const stamp = (Date.now() % 100000).toString().padStart(5, '0');
        send({ ts: stamp, fn: fn, src: srcRange.name + '+0x' + offset.toString(16), args: args, ret: retval });
    }

    function safeRead(fn, p, max) {
        try {
            if (!p || p.isNull || p.isNull()) return null;
            return fn(p, max);
        } catch (e) { return null; }
    }
    function readCStr(p, max) { try { return p && !p.isNull() ? Memory.readUtf8String(p, max || 256) : null; } catch (e) { return null; } }
    function readBytes(p, n)  { try { return p && !p.isNull() ? Memory.readByteArray(p, n) : null; } catch (e) { return null; } }

    function hook(name, opts) {
        const addr = Module.findExportByName(null, name);
        if (!addr) return false;
        Interceptor.attach(addr, {
            onEnter: function (args) {
                this._src = this.returnAddress;
                this._range = inTarget(this._src);
                if (!this._range) { this._skip = true; return; }
                this._argsCopy = opts.capture ? opts.capture(args, this) : null;
            },
            onLeave: function (retval) {
                if (this._skip) return;
                const out = opts.captureRet ? opts.captureRet(retval, this) : retval.toString();
                pushHit(name, this._argsCopy, out, this._range, this._src);
            }
        });
        return true;
    }

    function strArg(args, i, max) { return readCStr(args[i], max || 256); }
    function intArg(args, i)      { return args[i].toInt32(); }

    const installed = [];

    // ---- file-system probes (path strings) ----
    [
        ['open',     a => ({ path: strArg(a, 0), flags: '0x' + a[1].toString(16) })],
        ['__open_2', a => ({ path: strArg(a, 0), flags: '0x' + a[1].toString(16) })],
        ['openat',   a => ({ dirfd: intArg(a,0), path: strArg(a, 1), flags: '0x' + a[2].toString(16) })],
        ['__openat_2', a => ({ dirfd: intArg(a,0), path: strArg(a, 1) })],
        ['fopen',    a => ({ path: strArg(a, 0), mode: strArg(a, 1, 8) })],
        ['access',   a => ({ path: strArg(a, 0), mode: '0x' + a[1].toString(16) })],
        ['stat',     a => ({ path: strArg(a, 0) })],
        ['lstat',    a => ({ path: strArg(a, 0) })],
        ['fstatat',  a => ({ dirfd: intArg(a,0), path: strArg(a, 1) })],
        ['readlink', a => ({ path: strArg(a, 0) })],
        ['readlinkat', a => ({ dirfd: intArg(a,0), path: strArg(a, 1) })],
        ['opendir',  a => ({ path: strArg(a, 0) })],
        ['statfs',   a => ({ path: strArg(a, 0) })],
        ['unlink',   a => ({ path: strArg(a, 0) })],
        ['symlink',  a => ({ from: strArg(a, 0), to: strArg(a, 1) })],
        ['inotify_add_watch', a => ({ fd: intArg(a,0), path: strArg(a, 1), mask: '0x' + a[2].toString(16) })],
    ].forEach(([n, c]) => { if (hook(n, { capture: c })) installed.push(n); });

    // ---- module / symbol probes ----
    [
        ['dlopen',          a => ({ path: strArg(a, 0), flags: '0x' + a[1].toString(16) })],
        ['android_dlopen_ext', a => ({ path: strArg(a, 0), flags: '0x' + a[1].toString(16) })],
        ['dlsym',           a => ({ handle: a[0], sym: strArg(a, 1) })],
        ['dladdr',          a => ({ addr: a[0] })],
        ['dl_iterate_phdr', a => ({ cb: a[0] })],
    ].forEach(([n, c]) => { if (hook(n, { capture: c })) installed.push(n); });

    // ---- system properties ----
    [
        ['__system_property_get',  a => ({ name: strArg(a, 0) })],
        ['__system_property_find', a => ({ name: strArg(a, 0) })],
        ['__system_property_find_nth', a => ({ n: intArg(a,0) })],
        ['__system_property_read', a => ({ pi: a[0] })],
        ['__system_property_read_callback', a => ({ pi: a[0] })],
    ].forEach(([n, c]) => { if (hook(n, { capture: c })) installed.push(n); });

    // ---- anti-debug / process / signals ----
    [
        ['ptrace',  a => ({ req: '0x' + a[0].toString(16), pid: a[1].toInt32() })],
        ['prctl',   a => ({ op: '0x' + a[0].toString(16), a2: a[1], a3: a[2] })],
        ['kill',    a => ({ pid: a[0].toInt32(), sig: a[1].toInt32() })],
        ['tgkill',  a => ({ tgid: a[0].toInt32(), tid: a[1].toInt32(), sig: a[2].toInt32() })],
        ['raise',   a => ({ sig: a[0].toInt32() })],
        ['sigaction', a => ({ sig: a[0].toInt32() })],
        ['signal',  a => ({ sig: a[0].toInt32() })],
        ['fork',    a => ({})],
        ['vfork',   a => ({})],
        ['execv',   a => ({ path: strArg(a, 0) })],
        ['execve',  a => ({ path: strArg(a, 0) })],
        ['execl',   a => ({ path: strArg(a, 0) })],
        ['popen',   a => ({ cmd: strArg(a, 0, 512), mode: strArg(a, 1, 8) })],
        ['system',  a => ({ cmd: strArg(a, 0, 512) })],
        ['_exit',   a => ({ code: a[0].toInt32() })],
        ['exit',    a => ({ code: a[0].toInt32() })],
        ['abort',   a => ({})],
        ['waitpid', a => ({ pid: a[0].toInt32() })],
        ['syscall', a => ({ nr: a[0].toInt32() })],
    ].forEach(([n, c]) => { if (hook(n, { capture: c })) installed.push(n); });

    // ---- networking ----
    function sockaddrToStr(p) {
        try {
            if (!p || p.isNull()) return null;
            const family = Memory.readU16(p);
            if (family === 2) {       // AF_INET
                const port = (Memory.readU8(p.add(2)) << 8) | Memory.readU8(p.add(3));
                const ipBytes = [Memory.readU8(p.add(4)), Memory.readU8(p.add(5)), Memory.readU8(p.add(6)), Memory.readU8(p.add(7))];
                return `inet:${ipBytes.join('.')}:${port}`;
            } else if (family === 10) { // AF_INET6
                return 'inet6';
            } else if (family === 1) {  // AF_UNIX
                return 'unix:' + readCStr(p.add(2), 108);
            }
            return 'family=' + family;
        } catch (e) { return null; }
    }
    [
        ['socket',  a => ({ domain: a[0].toInt32(), type: a[1].toInt32(), proto: a[2].toInt32() })],
        ['connect', a => ({ fd: a[0].toInt32(), addr: sockaddrToStr(a[1]) })],
        ['bind',    a => ({ fd: a[0].toInt32(), addr: sockaddrToStr(a[1]) })],
        ['sendto',  a => ({ fd: a[0].toInt32(), addr: sockaddrToStr(a[4]) })],
        ['recvfrom',a => ({ fd: a[0].toInt32() })],
        ['getaddrinfo', a => ({ host: strArg(a, 0), service: strArg(a, 1) })],
    ].forEach(([n, c]) => { if (hook(n, { capture: c })) installed.push(n); });

    // ---- memory / integrity ----
    [
        ['mprotect', a => ({ addr: a[0], len: a[1].toInt32(), prot: '0x' + a[2].toString(16) })],
        ['mmap',     a => ({ len: a[1].toInt32(), prot: '0x' + a[2].toString(16), flags: '0x' + a[3].toString(16), fd: a[4].toInt32() })],
        ['mincore',  a => ({ addr: a[0], len: a[1].toInt32() })],
        ['madvise',  a => ({ addr: a[0], len: a[1].toInt32(), advice: a[2].toInt32() })],
        ['ioctl',    a => ({ fd: a[0].toInt32(), req: '0x' + a[1].toString(16) })],
    ].forEach(([n, c]) => { if (hook(n, { capture: c })) installed.push(n); });

    // ---- search APIs ----
    [
        ['memmem',   a => ({ haystackLen: a[1].toInt32(), needleLen: a[3].toInt32(), needle: readBytes(a[2], Math.min(64, a[3].toInt32())) })],
        ['strstr',   a => ({ needle: strArg(a, 1, 96) })],
        ['strcasestr', a => ({ needle: strArg(a, 1, 96) })],
        ['strstr',   a => ({ needle: strArg(a, 1, 96) })],
    ].forEach(([n, c]) => { if (hook(n, { capture: c })) installed.push(n); });

    send({ msg: 'probe-installed', count: installed.length, ranges: ranges.map(r => ({ name: r.name, base: r.lo.toString(), size: r.hi.sub(r.lo).toInt32() })), installed: installed });

    setTimeout(function () {
        send({ msg: 'probe-summary', total: total, distinct: seen.size });
    }, SECONDS_TO_RUN * 1000);
})();
