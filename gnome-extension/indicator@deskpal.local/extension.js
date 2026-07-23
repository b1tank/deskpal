/* exported init */
'use strict';

const {Clutter, Gio, GLib, St} = imports.gi;
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
        this._service = new IndicatorService();
    }

    disable() {
        if (this._service) {
            this._service.destroy();
            this._service = null;
        }
    }
}

function init() {
    return new DeskpalIndicatorExtension();
}
