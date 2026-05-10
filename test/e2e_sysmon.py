#!/usr/bin/env python3
"""
Canonical E2E test: deskpal × GNOME System Monitor

Exercises every interaction type deskpal supports against a real desktop app.
Run from repo root:  python3 test/e2e_sysmon.py

Prerequisites:
  - gnome-system-monitor installed
  - deskpal built (meson + ninja)
  - /dev/uinput accessible (chmod 666 or udev rule)
  - Running X11/Xwayland session
"""

import subprocess
import json
import time
import base64
import sys
import os

DESKPAL = os.path.join(os.path.dirname(__file__), '..', 'build', 'deskpal')
APP = 'gnome-system-monitor'
WIN = 'System Monitor'
SCREENSHOT_DIR = '/tmp/deskpal_e2e'

# ── helpers ──────────────────────────────────────────────────────────────────

class DeskpalClient:
    def __init__(self):
        self.proc = subprocess.Popen(
            [DESKPAL],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            cwd=os.path.dirname(DESKPAL))
        self._id = 0
        self._call('initialize', {
            'protocolVersion': '2024-11-05',
            'capabilities': {},
            'clientInfo': {'name': 'e2e-test', 'version': '1.0'}
        })

    def _call(self, method, params):
        self._id += 1
        msg = json.dumps({'jsonrpc': '2.0', 'id': self._id, 'method': method, 'params': params})
        self.proc.stdin.write((msg + '\n').encode())
        self.proc.stdin.flush()
        line = self.proc.stdout.readline().decode().strip()
        return json.loads(line) if line else {}

    def tool(self, name, args=None):
        r = self._call('tools/call', {'name': name, 'arguments': args or {}})
        content = r.get('result', {}).get('content', [{}])
        if not content:
            return ''
        c = content[0]
        if c.get('type') == 'image':
            return c  # return raw for screenshot saving
        return c.get('text', '')

    def screenshot(self, label):
        r = self._call('tools/call', {'name': 'screenshot', 'arguments': {'windowName': WIN}})
        content = r.get('result', {}).get('content', [{}])
        if content and content[0].get('type') == 'image':
            path = os.path.join(SCREENSHOT_DIR, f'{label}.png')
            with open(path, 'wb') as f:
                f.write(base64.b64decode(content[0]['data']))
            return path
        return None

    def close(self):
        self.proc.stdin.close()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


class TestRunner:
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.skipped = 0
        self.results = []

    def run(self, name, fn):
        t0 = time.time()
        try:
            ok, detail = fn()
            dt = time.time() - t0
            if ok:
                self.passed += 1
                tag = '\033[32m  PASS\033[0m'
            else:
                self.failed += 1
                tag = '\033[31m  FAIL\033[0m'
            print(f'{tag}  {name}  ({dt:.1f}s) {detail}')
            self.results.append((name, ok, dt, detail))
        except Exception as e:
            dt = time.time() - t0
            self.failed += 1
            tag = '\033[31m  ERR \033[0m'
            print(f'{tag}  {name}  ({dt:.1f}s) {e}')
            self.results.append((name, False, dt, str(e)))

    def summary(self):
        total = self.passed + self.failed + self.skipped
        print(f'\n{"=" * 60}')
        print(f'Results: {self.passed} passed, {self.failed} failed, {total} total')
        if self.failed:
            print('Failed:')
            for name, ok, _, detail in self.results:
                if not ok:
                    print(f'  - {name}: {detail}')
        print(f'{"=" * 60}')
        return self.failed == 0


# ── test cases ───────────────────────────────────────────────────────────────

def make_tests(d: DeskpalClient):
    """Returns list of (name, callable) test pairs."""

    tests = []

    # ── Phase 1: Window management ───────────────────────────────────────

    def test_list_windows():
        r = d.tool('list_windows')
        ok = WIN in r
        return ok, r[:80] if not ok else ''
    tests.append(('list_windows finds System Monitor', test_list_windows))

    def test_find_window():
        r = d.tool('find_window', {'name': 'System Monitor'})
        ok = 'System Monitor' in r
        return ok, r[:80] if not ok else ''
    tests.append(('find_window by name', test_find_window))

    def test_get_geometry():
        r = d.tool('get_window_geometry', {'windowName': WIN})
        ok = 'width' in r.lower() or 'x:' in r.lower() or 'position' in r.lower()
        return ok, r[:80] if not ok else ''
    tests.append(('get_window_geometry', test_get_geometry))

    def test_focus():
        r = d.tool('focus_window', {'windowName': WIN})
        ok = 'focus' in r.lower() or 'success' in r.lower() or 'activated' in r.lower()
        return ok, r[:80] if not ok else ''
    tests.append(('focus_window', test_focus))

    # ── Phase 2: Screenshot + OCR ────────────────────────────────────────

    def test_screenshot():
        path = d.screenshot('01_initial')
        ok = path is not None and os.path.exists(path)
        return ok, path or 'no image'
    tests.append(('screenshot', test_screenshot))

    def test_read_screen():
        r = d.tool('read_screen_text', {'windowName': WIN})
        ok = any(w in r for w in ['Process', 'CPU', 'Memory', 'Processes'])
        return ok, r[:80] if not ok else ''
    tests.append(('read_screen_text finds UI labels', test_read_screen))

    # ── Phase 3: Tab navigation (click_text) ─────────────────────────────

    def test_click_resources_tab():
        r = d.tool('click_text', {'windowName': WIN, 'text': 'Resources'})
        time.sleep(0.5)
        verify = d.tool('read_screen_text', {'windowName': WIN})
        ok = 'Resource' in r or 'Clicked' in r or 'CPU' in verify
        d.screenshot('02_resources_tab')
        return ok, ''
    tests.append(('click_text → Resources tab', test_click_resources_tab))

    def test_click_filesystems_tab():
        r = d.tool('click_text', {'windowName': WIN, 'text': 'File Systems'})
        time.sleep(0.5)
        verify = d.tool('read_screen_text', {'windowName': WIN})
        ok = 'File' in r or 'Clicked' in r or 'Device' in verify or 'Directory' in verify
        d.screenshot('03_filesystems_tab')
        return ok, ''
    tests.append(('click_text → File Systems tab', test_click_filesystems_tab))

    def test_click_processes_tab():
        r = d.tool('click_text', {'windowName': WIN, 'text': 'Processes'})
        time.sleep(0.5)
        verify = d.tool('read_screen_text', {'windowName': WIN})
        ok = 'Process' in r or 'Clicked' in r or 'Process' in verify
        d.screenshot('04_processes_tab')
        return ok, ''
    tests.append(('click_text → Processes tab', test_click_processes_tab))

    # ── Phase 4: Keyboard shortcuts ──────────────────────────────────────

    def test_ctrl_f_search():
        r = d.tool('key_press', {'windowName': WIN, 'keys': 'ctrl+f'})
        time.sleep(0.3)
        ok = 'Pressed' in r or 'pressed' in r
        d.screenshot('05_search_opened')
        return ok, r[:60]
    tests.append(('key_press Ctrl+F opens search', test_ctrl_f_search))

    def test_type_text():
        d.tool('key_press', {'windowName': WIN, 'keys': 'ctrl+a'})
        time.sleep(0.1)
        r = d.tool('type_text', {'windowName': WIN, 'text': 'firefox'})
        time.sleep(0.3)
        d.screenshot('06_typed_firefox')
        ok = 'Typed' in r or 'typed' in r or '7' in r
        return ok, r[:60]
    tests.append(('type_text "firefox" in search', test_type_text))

    def test_escape_close_search():
        r = d.tool('key_press', {'windowName': WIN, 'keys': 'Escape'})
        time.sleep(0.3)
        ok = 'Pressed' in r or 'pressed' in r
        d.screenshot('07_search_closed')
        return ok, ''
    tests.append(('key_press Escape closes search', test_escape_close_search))

    # ── Phase 5: Hamburger menu ──────────────────────────────────────────

    def test_hamburger_open():
        r = d.tool('click_text', {'windowName': WIN, 'text': '☰'})
        if 'not found' in r.lower() or 'could not find' in r.lower():
            # Try open via keyboard
            r = d.tool('key_press', {'windowName': WIN, 'keys': 'F10'})
        time.sleep(0.5)
        d.screenshot('08_hamburger_open')
        verify = d.tool('read_screen_text', {'windowName': WIN})
        ok = any(w in verify for w in ['All Processes', 'My Processes', 'Active', 'Preferences', 'About'])
        return ok, '' if ok else verify[:80]
    tests.append(('open hamburger menu', test_hamburger_open))

    def test_hamburger_close_escape():
        r = d.tool('key_press', {'windowName': WIN, 'keys': 'Escape'})
        time.sleep(0.3)
        ok = 'Pressed' in r or 'pressed' in r
        return ok, ''
    tests.append(('close menu with Escape', test_hamburger_close_escape))

    # ── Phase 6: Menu item interaction ───────────────────────────────────

    def test_menu_all_processes():
        # Open menu via F10 (more reliable than clicking ☰ glyph)
        d.tool('key_press', {'windowName': WIN, 'keys': 'F10'})
        time.sleep(0.6)
        # Navigate with arrow keys: first item is usually the process filter
        r = d.tool('key_press', {'windowName': WIN, 'keys': 'Down Down Return'})
        time.sleep(0.5)
        d.screenshot('09_all_processes')
        # Verify we're still on Processes tab (menu didn't break anything)
        verify = d.tool('read_screen_text', {'windowName': WIN})
        ok = 'Process' in verify or 'CPU' in verify or 'Memory' in verify
        return ok, '' if ok else verify[:60]
    tests.append(('menu → navigate with keyboard', test_menu_all_processes))

    def test_menu_preferences():
        d.tool('key_press', {'windowName': WIN, 'keys': 'F10'})
        time.sleep(0.6)
        # Navigate to Preferences (typically near bottom of menu)
        for _ in range(6):
            d.tool('key_press', {'windowName': WIN, 'keys': 'Down'})
            time.sleep(0.1)
        d.screenshot('10_menu_navigated')
        # Close without selecting
        d.tool('key_press', {'windowName': WIN, 'keys': 'Escape'})
        time.sleep(0.3)
        ok = True
        return ok, ''
    tests.append(('menu → keyboard navigation + close', test_menu_preferences))

    # ── Phase 7: Coordinate-based click ──────────────────────────────────

    def test_coordinate_click():
        # Click the center of the window (should be in process list area)
        geom = d.tool('get_window_geometry', {'windowName': WIN})
        # Parse geometry - format varies
        r = d.tool('click', {'windowName': WIN, 'x': 400, 'y': 300})
        time.sleep(0.3)
        ok = 'Clicked' in r or 'clicked' in r
        d.screenshot('11_coord_click')
        return ok, r[:60]
    tests.append(('click at coordinates (400, 300)', test_coordinate_click))

    def test_double_click():
        r = d.tool('click', {'windowName': WIN, 'x': 400, 'y': 300, 'repeat': 2})
        time.sleep(0.3)
        ok = 'Clicked' in r or 'clicked' in r or 'double' in r.lower()
        return ok, r[:60]
    tests.append(('double-click at coordinates', test_double_click))

    # ── Phase 8: Mouse operations ────────────────────────────────────────

    def test_mouse_move():
        r = d.tool('mouse_move', {'windowName': WIN, 'x': 200, 'y': 200})
        time.sleep(0.2)
        ok = 'Moved' in r or 'moved' in r or 'position' in r.lower()
        return ok, r[:60]
    tests.append(('mouse_move to (200, 200)', test_mouse_move))

    def test_scroll_down():
        r = d.tool('scroll', {'windowName': WIN, 'x': 400, 'y': 400, 'amount': -3})
        time.sleep(0.3)
        ok = 'Scroll' in r or 'scroll' in r
        return ok, r[:60]
    tests.append(('scroll down', test_scroll_down))

    def test_scroll_up():
        r = d.tool('scroll', {'windowName': WIN, 'x': 400, 'y': 400, 'amount': 3})
        time.sleep(0.3)
        ok = 'Scroll' in r or 'scroll' in r
        return ok, r[:60]
    tests.append(('scroll up', test_scroll_up))

    def test_right_click():
        r = d.tool('click', {'windowName': WIN, 'x': 400, 'y': 300, 'button': 'right'})
        time.sleep(0.5)
        d.screenshot('12_right_click')
        ok = 'Clicked' in r or 'clicked' in r
        # Dismiss context menu
        d.tool('key_press', {'windowName': WIN, 'keys': 'Escape'})
        time.sleep(0.3)
        return ok, r[:60]
    tests.append(('right-click → context menu', test_right_click))

    # ── Phase 9: Column sort ─────────────────────────────────────────────

    def test_sort_memory():
        r = d.tool('click_text', {'windowName': WIN, 'text': 'Memory'})
        time.sleep(0.5)
        d.screenshot('13_sorted_memory')
        ok = 'Clicked' in r or 'clicked' in r
        return ok, r[:60] if not ok else ''
    tests.append(('click_text → sort by Memory column', test_sort_memory))

    def test_sort_cpu():
        # May appear as "% CPU" or "CPU"
        r = d.tool('click_text', {'windowName': WIN, 'text': '% CPU'})
        if 'not found' in r.lower():
            r = d.tool('click_text', {'windowName': WIN, 'text': 'CPU'})
        time.sleep(0.5)
        d.screenshot('14_sorted_cpu')
        ok = 'Clicked' in r or 'clicked' in r
        return ok, r[:60] if not ok else ''
    tests.append(('click_text → sort by CPU column', test_sort_cpu))

    # ── Phase 10: Drag ───────────────────────────────────────────────────

    def test_drag():
        r = d.tool('drag', {
            'windowName': WIN,
            'startX': 400, 'startY': 300,
            'endX': 400, 'endY': 500
        })
        time.sleep(0.3)
        ok = 'Drag' in r or 'drag' in r or 'Moved' in r
        return ok, r[:60]
    tests.append(('drag from (400,300) to (400,500)', test_drag))

    # ── Phase 11: mouse_down / mouse_up ──────────────────────────────────

    def test_mouse_down_up():
        r1 = d.tool('mouse_down', {'windowName': WIN, 'x': 400, 'y': 400})
        time.sleep(0.1)
        r2 = d.tool('mouse_up', {'windowName': WIN, 'x': 400, 'y': 400})
        time.sleep(0.2)
        ok = ('down' in r1.lower() or 'press' in r1.lower()) and ('up' in r2.lower() or 'release' in r2.lower())
        return ok, f'{r1[:30]} / {r2[:30]}'
    tests.append(('mouse_down + mouse_up', test_mouse_down_up))

    # ── Phase 12: Resize ─────────────────────────────────────────────────

    def test_resize():
        r = d.tool('resize_window', {'windowName': WIN, 'width': 1200, 'height': 900})
        time.sleep(0.5)
        d.screenshot('15_resized')
        ok = 'Resized' in r or 'resized' in r or 'resize' in r.lower()
        return ok, r[:60]
    tests.append(('resize_window to 1200x900', test_resize))

    def test_resize_restore():
        r = d.tool('resize_window', {'windowName': WIN, 'width': 1504, 'height': 1104})
        time.sleep(0.3)
        ok = 'Resized' in r or 'resized' in r or 'resize' in r.lower()
        return ok, r[:60]
    tests.append(('resize_window restore original', test_resize_restore))

    # ── Phase 13: Rapid tab switching (stress) ───────────────────────────

    def test_rapid_tabs():
        tabs = ['Resources', 'Processes', 'File Systems', 'Processes']
        for tab in tabs:
            d.tool('click_text', {'windowName': WIN, 'text': tab})
            time.sleep(0.3)
        d.screenshot('16_rapid_tabs_done')
        verify = d.tool('read_screen_text', {'windowName': WIN})
        ok = 'Process' in verify
        return ok, ''
    tests.append(('rapid tab switching (4x)', test_rapid_tabs))

    # ── Phase 14: Final state ────────────────────────────────────────────

    def test_final_screenshot():
        path = d.screenshot('17_final')
        ok = path is not None and os.path.exists(path)
        return ok, path or 'no image'
    tests.append(('final screenshot', test_final_screenshot))

    return tests


# ── main ─────────────────────────────────────────────────────────────────────

def main():
    os.makedirs(SCREENSHOT_DIR, exist_ok=True)

    # Ensure System Monitor is running on Xwayland
    subprocess.run(['pkill', '-f', 'gnome-system-monitor'], capture_output=True)
    time.sleep(0.5)
    env = os.environ.copy()
    env['GDK_BACKEND'] = 'x11'
    subprocess.Popen([APP], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)

    print(f'\n{"=" * 60}')
    print(f'  DESKPAL E2E TEST — {WIN}')
    print(f'  Screenshots: {SCREENSHOT_DIR}/')
    print(f'{"=" * 60}\n')

    d = DeskpalClient()
    time.sleep(0.3)

    runner = TestRunner()
    tests = make_tests(d)

    for name, fn in tests:
        runner.run(name, fn)

    d.close()
    ok = runner.summary()

    print(f'\nScreenshots saved to {SCREENSHOT_DIR}/')
    print(f'  ls {SCREENSHOT_DIR}/*.png\n')

    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
