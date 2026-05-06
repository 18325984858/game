#!/usr/bin/env python3
"""Driver to run probe.js against a Tencent ACE-protected app and capture results."""
import argparse, json, os, sys, time, frida


def parse():
    ap = argparse.ArgumentParser()
    ap.add_argument('--name', help='process name (e.g. com.tencent.tmgp.dfm)')
    ap.add_argument('--pid',  type=int, help='process pid')
    ap.add_argument('--script', default=os.path.join(os.path.dirname(__file__), 'probe.js'))
    ap.add_argument('--out',    default=os.path.join(os.path.dirname(__file__), 'detections.jsonl'))
    ap.add_argument('--seconds', type=int, default=30)
    return ap.parse_args()


def main():
    args = parse()
    dev = frida.get_usb_device(timeout=5)
    target = args.pid if args.pid else args.name
    if target is None:
        print('need --name or --pid', file=sys.stderr); sys.exit(2)

    sess = dev.attach(target)
    print(f'[+] attached: {target}', flush=True)
    try:
        sess.enable_jit()
    except Exception:
        pass

    out = open(args.out, 'w', encoding='utf-8')
    counts = {}

    def on_msg(message, data):
        if message.get('type') == 'send':
            payload = message.get('payload')
            if isinstance(payload, dict) and 'fn' in payload:
                counts[payload['fn']] = counts.get(payload['fn'], 0) + 1
            print(json.dumps(payload, ensure_ascii=False), flush=True)
            out.write(json.dumps(payload, ensure_ascii=False) + '\n')
            out.flush()
        elif message.get('type') == 'error':
            print('[!] frida error:', message.get('description'), flush=True)

    with open(args.script, 'r', encoding='utf-8') as f:
        src = f.read()
    script = sess.create_script(src, runtime='qjs')
    script.on('message', on_msg)
    script.load()
    print('[+] script loaded; tracing for', args.seconds, 'sec', flush=True)
    time.sleep(args.seconds)

    print('\n=== top counters ===')
    for k, v in sorted(counts.items(), key=lambda kv: -kv[1])[:60]:
        print(f'{v:6}  {k}')
    out.close()
    sess.detach()


if __name__ == '__main__':
    main()
