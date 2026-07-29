/* exported init */
'use strict';

const {Clutter, Gio, GLib, Meta, St} = imports.gi;
const Main = imports.ui.main;

const SERVICE_NAME = 'org.deskpal.Indicator';
const OBJECT_PATH = '/org/deskpal/Indicator';
const INTERFACE_NAME = 'org.deskpal.Indicator';

const INTERFACE_XML = `
<node>
  <interface name="${INTERFACE_NAME}">
    <method name="Ping">
      <arg name="version" type="s" direction="out"/>
    </method>
    <method name="GetStatus">
      <arg name="json" type="s" direction="out"/>
    </method>
    <method name="ShowCursor">
      <arg name="cursor_id" type="s" direction="in"/>
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
      <arg name="color" type="s" direction="in"/>
      <arg name="label" type="s" direction="in"/>
      <arg name="shown" type="b" direction="out"/>
    </method>
    <method name="MoveCursor">
      <arg name="cursor_id" type="s" direction="in"/>
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
      <arg name="moved" type="b" direction="out"/>
    </method>
    <method name="MoveCursorStyled">
      <arg name="cursor_id" type="s" direction="in"/>
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
      <arg name="color" type="s" direction="in"/>
      <arg name="label" type="s" direction="in"/>
      <arg name="moved" type="b" direction="out"/>
    </method>
    <method name="HideCursor">
      <arg name="cursor_id" type="s" direction="in"/>
      <arg name="hidden" type="b" direction="out"/>
    </method>
    <method name="ClearAll"/>
    <method name="ListCursors">
      <arg name="json" type="s" direction="out"/>
    </method>
  </interface>
</node>`;

const SHELL_BRIDGE_SERVICE = 'org.deskpal.ShellBridge';
const SHELL_BRIDGE_PATH = '/org/deskpal/ShellBridge';
const SHELL_BRIDGE_INTERFACE = 'org.deskpal.ShellBridge1';
const SHELL_BRIDGE_PROTOCOL_VERSION = 1;
const SHELL_BRIDGE_MAX_WINDOWS = 256;
const SHELL_BRIDGE_MAX_STRING_CHARACTERS = 512;

const SHELL_BRIDGE_XML = `
<node>
  <interface name="${SHELL_BRIDGE_INTERFACE}">
    <method name="GetCapabilities">
      <arg name="json" type="s" direction="out"/>
    </method>
    <method name="ListWindows">
      <arg name="json" type="s" direction="out"/>
    </method>
    <method name="GetMonitorLayout">
      <arg name="json" type="s" direction="out"/>
    </method>
  </interface>
</node>`;

function validCursorId(value) {
    return typeof value === 'string' &&
        value.length > 0 && value.length <= 64 &&
        /^[A-Za-z0-9_.-]+$/.test(value);
}

function safeColor(value) {
    return typeof value === 'string' && /^#[0-9A-Fa-f]{6}$/.test(value)
        ? value
        : '#36C5F0';
}

function safeLabel(value, cursorId) {
    if (typeof value !== 'string' || value.length === 0)
        return cursorId;
    return value.replace(/[\x00-\x1F\x7F]/g, ' ').slice(0, 48);
}

function colorChannels(value) {
    const color = safeColor(value);
    return [1, 3, 5].map(offset =>
        Number.parseInt(color.slice(offset, offset + 2), 16) / 255);
}

class LogicalCursor {
    constructor(cursorId, owner, x, y, color, label) {
        this.cursorId = cursorId;
        this.owner = owner;
        this.x = x;
        this.y = y;
        this.color = safeColor(color);
        this.label = safeLabel(label, cursorId);
        this.sequence = 0;
        this.state = 'idle';

        this.actor = new St.BoxLayout({
            style_class: 'deskpal-agent-cursor',
            reactive: false,
            can_focus: false,
            track_hover: false,
        });
        this.pointer = new St.DrawingArea({
            style_class: 'deskpal-agent-cursor-pointer',
            reactive: false,
            width: 25,
            height: 33,
        });
        this.pointer.connect('repaint', area => this._repaintPointer(area));
        this.caption = new St.Label({
            style_class: 'deskpal-agent-cursor-label',
            text: this.label,
            reactive: false,
            style: `border-color: ${this.color};`,
        });
        this.actor.add_child(this.pointer);
        this.actor.add_child(this.caption);

        Main.layoutManager.addChrome(this.actor, {
            affectsInputRegion: false,
            affectsStruts: false,
            trackFullscreen: true,
        });
        this.move(x, y, false);
    }

    setStyle(color, label) {
        this.color = safeColor(color);
        this.label = safeLabel(label, this.cursorId);
        this.pointer.queue_repaint();
        this.caption.set_style(`border-color: ${this.color};`);
        this.caption.set_text(this.label);
    }

    show(x, y, color, label) {
        this.setStyle(color, label);
        this.actor.show();
        this.move(x, y, false);
    }

    _repaintPointer(area) {
        const context = area.get_context();
        const [red, green, blue] = colorChannels(this.color);

        // Familiar northwest-pointing mouse cursor. D-Bus coordinates identify
        // the pointer tip at the actor's top-left corner.
        context.moveTo(1.5, 1.5);
        context.lineTo(1.5, 28);
        context.lineTo(8, 21.5);
        context.lineTo(13, 31);
        context.lineTo(18, 28.5);
        context.lineTo(13, 19.5);
        context.lineTo(23, 19);
        context.closePath();
        context.setSourceRGBA(red, green, blue, 1);
        context.fillPreserve();
        context.setSourceRGBA(1, 1, 1, 0.98);
        context.setLineWidth(2);
        context.stroke();
        context.$dispose();
    }

    move(x, y, animate = true) {
        const [stageWidth, stageHeight] = global.stage.get_size();
        this.x = Math.max(0, Math.min(x, stageWidth - 1));
        this.y = Math.max(0, Math.min(y, stageHeight - 1));
        this.sequence++;
        const sequence = this.sequence;
        this.actor.remove_all_transitions();
        if (animate) {
            this.state = 'moving';
            this.actor.ease({
                x: this.x,
                y: this.y,
                duration: 180,
                mode: Clutter.AnimationMode.EASE_OUT_QUAD,
                onComplete: () => {
                    if (this.sequence === sequence)
                        this.state = 'idle';
                },
            });
        } else {
            this.actor.set_position(this.x, this.y);
            this.state = 'idle';
        }
    }

    destroy() {
        this.actor.remove_all_transitions();
        this.actor.destroy();
    }

    toJSON() {
        return {
            cursorId: this.cursorId,
            x: this.x,
            y: this.y,
            color: this.color,
            label: this.label,
            sequence: this.sequence,
            state: this.state,
            renderedX: Math.round(this.actor.x),
            renderedY: Math.round(this.actor.y),
        };
    }
}

function boundedShellString(value) {
    if (value === null || value === undefined)
        return null;
    return String(value)
        .replace(/[\x00-\x1F\x7F]/g, ' ')
        .slice(0, SHELL_BRIDGE_MAX_STRING_CHARACTERS);
}

function shellValue(object, method, fallback = null) {
    if (!object || typeof object[method] !== 'function')
        return fallback;
    return object[method]();
}

function shellClientType(window) {
    const value = shellValue(window, 'get_client_type');
    if (value === Meta.WindowClientType.WAYLAND)
        return 'wayland';
    if (value === Meta.WindowClientType.X11)
        return 'x11';
    return 'unknown';
}

class ShellBridgeService {
    constructor() {
        this._shellInstanceId = GLib.uuid_string_random();
        this._nextSurfaceId = 1;
        this._windows = new Map();
        this._dbusObject = Gio.DBusExportedObject.wrapJSObject(
            SHELL_BRIDGE_XML, this);
        this._dbusObject.export(Gio.DBus.session, SHELL_BRIDGE_PATH);
        this._nameId = Gio.bus_own_name_on_connection(
            Gio.DBus.session,
            SHELL_BRIDGE_SERVICE,
            Gio.BusNameOwnerFlags.NONE,
            null,
            null);
    }

    _monitorScale(index) {
        try {
            return global.display.get_monitor_scale(index);
        } catch (_error) {
            return St.ThemeContext.get_for_stage(global.stage).scale_factor;
        }
    }

    _monitorLayout() {
        const primaryIndex = Main.layoutManager.primaryIndex;
        return Main.layoutManager.monitors.map((monitor, index) => ({
            index,
            x: monitor.x,
            y: monitor.y,
            width: monitor.width,
            height: monitor.height,
            scale: this._monitorScale(index),
            primary: index === primaryIndex,
        }));
    }

    _liveWindows() {
        return global.get_window_actors()
            .map(actor => actor.meta_window)
            .filter(window => window &&
                !shellValue(window, 'is_override_redirect', false))
            .filter(window =>
                shellValue(window, 'get_window_type') !== Meta.WindowType.DESKTOP);
    }

    _identityFor(window, geometrySignature) {
        let identity = this._windows.get(window);
        if (!identity) {
            identity = {
                surfaceId: `gnome-window-${this._nextSurfaceId++}`,
                generation: 1,
                geometryRevision: 1,
                geometrySignature,
            };
            this._windows.set(window, identity);
        } else if (identity.geometrySignature !== geometrySignature) {
            identity.geometrySignature = geometrySignature;
            identity.geometryRevision++;
        }
        return identity;
    }

    _windowInfo(window) {
        const rect = window.get_frame_rect();
        if (!rect || rect.width <= 0 || rect.height <= 0)
            return null;
        const workspace = shellValue(window, 'get_workspace');
        const workspaceIndex = shellValue(workspace, 'index', -1);
        const monitor = shellValue(window, 'get_monitor', -1);
        const scale = monitor >= 0 ? this._monitorScale(monitor) : 1;
        const geometrySignature = [
            rect.x, rect.y, rect.width, rect.height,
            workspaceIndex, monitor, scale,
        ].join(':');
        const identity = this._identityFor(window, geometrySignature);
        const app = Shell.WindowTracker.get_default().get_window_app(window);
        return {
            surfaceId: identity.surfaceId,
            generation: identity.generation,
            geometryRevision: identity.geometryRevision,
            title: boundedShellString(shellValue(window, 'get_title')),
            appId: boundedShellString(shellValue(app, 'get_id')),
            wmClass: boundedShellString(shellValue(window, 'get_wm_class')),
            pid: shellValue(window, 'get_pid'),
            bounds: {
                x: rect.x,
                y: rect.y,
                width: rect.width,
                height: rect.height,
            },
            workspace: workspaceIndex >= 0 ? workspaceIndex : null,
            monitor: monitor >= 0 ? monitor : null,
            scale,
            focused: global.display.focus_window === window && !Main.overview.visible,
            hidden: window.minimized === true,
            clientType: shellClientType(window),
            backend: 'gnome-shell-extension',
        };
    }

    GetCapabilities() {
        return JSON.stringify({
            protocolVersion: SHELL_BRIDGE_PROTOCOL_VERSION,
            backend: 'gnome-shell-extension',
            shellInstanceId: this._shellInstanceId,
            coordinateSpace: 'gnome-stage-logical',
            capabilities: {
                windowEnumeration: true,
                monitorLayout: true,
                windowCapture: false,
                foregroundWindowManagement: false,
                surfaceInput: false,
                backgroundInput: false,
            },
            limits: {
                maxWindows: SHELL_BRIDGE_MAX_WINDOWS,
                maxStringCharacters: SHELL_BRIDGE_MAX_STRING_CHARACTERS,
            },
        });
    }

    ListWindows() {
        const live = this._liveWindows();
        const liveSet = new Set(live);
        for (const window of this._windows.keys()) {
            if (!liveSet.has(window))
                this._windows.delete(window);
        }
        const windows = live.slice(0, SHELL_BRIDGE_MAX_WINDOWS)
            .map(window => this._windowInfo(window))
            .filter(window => window !== null);
        const complete = live.length <= SHELL_BRIDGE_MAX_WINDOWS &&
            windows.length === live.length;
        return JSON.stringify({
            protocolVersion: SHELL_BRIDGE_PROTOCOL_VERSION,
            shellInstanceId: this._shellInstanceId,
            complete,
            windows,
        });
    }

    GetMonitorLayout() {
        const [stageWidth, stageHeight] = global.stage.get_size();
        return JSON.stringify({
            protocolVersion: SHELL_BRIDGE_PROTOCOL_VERSION,
            shellInstanceId: this._shellInstanceId,
            coordinateSpace: 'gnome-stage-logical',
            stageWidth,
            stageHeight,
            primaryIndex: Main.layoutManager.primaryIndex,
            complete: true,
            monitors: this._monitorLayout(),
        });
    }

    destroy() {
        this._windows.clear();
        if (this._nameId) {
            Gio.bus_unown_name(this._nameId);
            this._nameId = 0;
        }
        if (this._dbusObject) {
            this._dbusObject.unexport();
            this._dbusObject = null;
        }
    }
}

class IndicatorService {
    constructor() {
        this._cursors = new Map();
        this._dbusObject = Gio.DBusExportedObject.wrapJSObject(
            INTERFACE_XML, this);
        this._dbusObject.export(Gio.DBus.session, OBJECT_PATH);
        this._nameId = Gio.bus_own_name_on_connection(
            Gio.DBus.session,
            SERVICE_NAME,
            Gio.BusNameOwnerFlags.NONE,
            null,
            null);
        this._nameOwnerChangedId = Gio.DBus.session.signal_subscribe(
            'org.freedesktop.DBus',
            'org.freedesktop.DBus',
            'NameOwnerChanged',
            '/org/freedesktop/DBus',
            null,
            Gio.DBusSignalFlags.NONE,
            (_connection, _sender, _path, _interface, _signal, parameters) => {
                const [name, oldOwner, newOwner] = parameters.deep_unpack();
                if (name.startsWith(':') && oldOwner && !newOwner)
                    this._removeOwner(name);
            });
    }

    Ping() {
        return 'deskpal-indicator-v1';
    }

    GetStatus() {
        const [stageWidth, stageHeight] = global.stage.get_size();
        const primaryIndex = Main.layoutManager.primaryIndex;
        const monitors = Main.layoutManager.monitors.map((monitor, index) => {
            let scale = St.ThemeContext.get_for_stage(global.stage).scale_factor;
            try {
                scale = global.display.get_monitor_scale(index);
            } catch (_error) {
                // GNOME versions without per-monitor scale use the stage scale.
            }
            return {
                index,
                x: monitor.x,
                y: monitor.y,
                width: monitor.width,
                height: monitor.height,
                scale,
                primary: index === primaryIndex,
            };
        });
        return JSON.stringify({
            version: 'deskpal-indicator-v1',
            coordinateSpace: 'gnome-stage-logical',
            stageWidth,
            stageHeight,
            monitors,
            cursors: Array.from(
                this._cursors.values(), cursor => cursor.toJSON()),
        });
    }

    _returnBoolean(invocation, value) {
        invocation.return_value(new GLib.Variant('(b)', [value]));
    }

    _senderCanMutate(cursor, sender) {
        return cursor.owner === null || cursor.owner === sender;
    }

    _removeOwner(owner) {
        for (const [cursorId, cursor] of this._cursors) {
            if (cursor.owner !== owner)
                continue;
            cursor.destroy();
            this._cursors.delete(cursorId);
        }
    }

    ShowCursorAsync(params, invocation) {
        const [cursorId, x, y, color, label] = params;
        if (!validCursorId(cursorId)) {
            this._returnBoolean(invocation, false);
            return;
        }

        const sender = invocation.get_sender();
        let cursor = this._cursors.get(cursorId);
        if (cursor && !this._senderCanMutate(cursor, sender)) {
            this._returnBoolean(invocation, false);
            return;
        }
        if (!cursor) {
            // Deskpal-owned IDs are tied to the caller's unique D-Bus name and
            // disappear when that connection dies. Unprefixed IDs retain the
            // manual development/demo behavior of the original prototype.
            const owner = cursorId.startsWith('dp-') ? sender : null;
            cursor = new LogicalCursor(cursorId, owner, x, y, color, label);
            this._cursors.set(cursorId, cursor);
        } else {
            cursor.show(x, y, color, label);
        }
        this._returnBoolean(invocation, true);
    }

    MoveCursorAsync(params, invocation) {
        const [cursorId, x, y] = params;
        const cursor = this._cursors.get(cursorId);
        if (!cursor || !this._senderCanMutate(cursor, invocation.get_sender())) {
            this._returnBoolean(invocation, false);
            return;
        }
        cursor.move(x, y);
        this._returnBoolean(invocation, true);
    }

    MoveCursorStyledAsync(params, invocation) {
        const [cursorId, x, y, color, label] = params;
        const cursor = this._cursors.get(cursorId);
        if (!cursor || !this._senderCanMutate(cursor, invocation.get_sender())) {
            this._returnBoolean(invocation, false);
            return;
        }
        cursor.setStyle(color, label);
        cursor.move(x, y);
        this._returnBoolean(invocation, true);
    }

    HideCursorAsync(params, invocation) {
        const [cursorId] = params;
        const cursor = this._cursors.get(cursorId);
        if (!cursor || !this._senderCanMutate(cursor, invocation.get_sender())) {
            this._returnBoolean(invocation, false);
            return;
        }
        cursor.destroy();
        this._cursors.delete(cursorId);
        this._returnBoolean(invocation, true);
    }

    ClearAllAsync(_params, invocation) {
        const sender = invocation.get_sender();
        for (const [cursorId, cursor] of this._cursors) {
            if (!this._senderCanMutate(cursor, sender))
                continue;
            cursor.destroy();
            this._cursors.delete(cursorId);
        }
        invocation.return_value(null);
    }

    ListCursors() {
        return JSON.stringify(Array.from(
            this._cursors.values(), cursor => cursor.toJSON()));
    }

    destroy() {
        for (const cursor of this._cursors.values())
            cursor.destroy();
        this._cursors.clear();
        if (this._nameOwnerChangedId) {
            Gio.DBus.session.signal_unsubscribe(this._nameOwnerChangedId);
            this._nameOwnerChangedId = 0;
        }
        if (this._nameId) {
            Gio.bus_unown_name(this._nameId);
            this._nameId = 0;
        }
        if (this._dbusObject) {
            this._dbusObject.unexport();
            this._dbusObject = null;
        }
    }
}

class DeskpalIndicatorExtension {
    enable() {
        this._indicatorService = new IndicatorService();
        this._shellBridgeService = new ShellBridgeService();
    }

    disable() {
        if (this._shellBridgeService) {
            this._shellBridgeService.destroy();
            this._shellBridgeService = null;
        }
        if (this._indicatorService) {
            this._indicatorService.destroy();
            this._indicatorService = null;
        }
    }
}

function init() {
    return new DeskpalIndicatorExtension();
}
