# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Interactive render in the render window, as the IPR of a frame buffer.

While it is on, the scene is rendered into the Render Result as with Render Image, and rendered
again whenever something that shows in the render changes: objects, materials, lights, the
camera, the frame, the render resolution or the render region. So it uses the final render
settings, the resolution percentage and the region, and the result can be saved as any render.

Persistent Data is turned on meanwhile, so engines that support it (Cycles) keep the scene
between renders and only update what changed. Lock Interface is turned off, as the scene has to
be edited while it renders. Both are put back when the interactive render is stopped.

Pause (`RenderSettings.render_pause`) holds the render in progress; changes made meanwhile are
rendered when it is resumed.
"""

import bpy
from bpy.types import Operator

# How often changes are looked for, in seconds. Also the delay before a render starts again, so
# that dragging a value does not restart the render for every step.
_INTERVAL = 0.03


class _State:
    scene_name = ""
    # A change was seen: render again.
    dirty = False
    # The render in progress was asked to stop, to start the next one.
    stop_requested = False
    # Settings of the scene that have no dependency graph update, to see them change.
    signature = None
    # Options to put back when stopped.
    restore = {}


def is_active():
    """True while the interactive render is on (the timer is gone after loading a file)."""
    return bool(_State.scene_name) and bpy.app.timers.is_registered(_tick)


def _scene():
    return bpy.data.scenes.get(_State.scene_name)


def _signature(scene):
    render = scene.render
    return (
        render.resolution_x, render.resolution_y, render.resolution_percentage,
        render.use_border, render.use_crop_to_border,
        render.border_min_x, render.border_max_x, render.border_min_y, render.border_max_y,
        scene.camera.name if scene.camera else "",
        render.engine,
    )


def _window(scene):
    for window in bpy.context.window_manager.windows:
        if window.scene == scene:
            return window
    return None


def _on_depsgraph_update(scene, depsgraph):
    if not _State.scene_name or scene.name != _State.scene_name:
        return
    # Renders evaluate their own dependency graph: only edits in the interface count.
    if depsgraph.mode != 'VIEWPORT':
        return
    for update in depsgraph.updates:
        # Selecting, the Render Result itself or the compositor (which the Light Mix runs again)
        # change nothing in the render.
        if isinstance(update.id, bpy.types.Image):
            continue
        if isinstance(update.id, bpy.types.NodeTree) and update.id.bl_idname == 'CompositorNodeTree':
            continue
        if update.is_updated_transform or update.is_updated_geometry or update.is_updated_shading:
            _State.dirty = True
            return


def _render(scene):
    window = _window(scene)
    if window is None:
        return False
    view = bpy.context.preferences.view
    display_type = view.render_display_type
    # Do not open or raise the render window for every change.
    view.render_display_type = 'NONE'
    try:
        with bpy.context.temp_override(window=window):
            bpy.ops.render.render('INVOKE_DEFAULT')
    finally:
        view.render_display_type = display_type
    return True


def _tick():
    scene = _scene()
    if scene is None:
        _State.scene_name = ""
        return None

    signature = _signature(scene)
    if signature != _State.signature:
        _State.signature = signature
        _State.dirty = True

    # While paused, changes wait for the render to be resumed.
    if _State.dirty and not scene.render.render_pause:
        if bpy.app.is_job_running('RENDER'):
            # Ask the render in progress to stop, and start the next one once it has. Waiting
            # for it here would freeze the whole interface for every change.
            if not _State.stop_requested:
                _State.stop_requested = True
                window = _window(scene)
                if window is not None:
                    with bpy.context.temp_override(window=window):
                        bpy.ops.render.render_stop(wait=False)
            return _INTERVAL
        _State.stop_requested = False
        _State.dirty = False
        if not _render(scene):
            # No window shows this scene any more. Returning None ends this timer.
            _stop(scene, from_timer=True)
            return None
    return _INTERVAL


def _start(scene):
    render = scene.render
    _State.restore = {
        "use_persistent_data": render.use_persistent_data,
        "use_lock_interface": render.use_lock_interface,
    }
    render.use_persistent_data = True
    render.use_lock_interface = False
    render.render_pause = False
    # No empty image between one render and the next.
    render.render_keep_result = True

    _State.scene_name = scene.name
    _State.signature = _signature(scene)
    _State.dirty = True
    _State.stop_requested = False

    handlers = bpy.app.handlers.depsgraph_update_post
    if _on_depsgraph_update not in handlers:
        handlers.append(_on_depsgraph_update)
    if not bpy.app.timers.is_registered(_tick):
        bpy.app.timers.register(_tick, first_interval=0.0)


def _stop(scene, *, from_timer=False):
    _State.scene_name = ""
    _State.dirty = False

    handlers = bpy.app.handlers.depsgraph_update_post
    if _on_depsgraph_update in handlers:
        handlers.remove(_on_depsgraph_update)
    if not from_timer and bpy.app.timers.is_registered(_tick):
        bpy.app.timers.unregister(_tick)

    if scene is not None:
        render = scene.render
        render.render_pause = False
        render.render_keep_result = False
        for attr, value in _State.restore.items():
            setattr(render, attr, value)
    _State.restore = {}


class RENDER_OT_interactive(Operator):
    """Render interactively: the Render Result is rendered again whenever the scene, """ \
        """the render resolution or the render region changes. Click again to stop"""
    bl_idname = "render.interactive"
    bl_label = "Interactive Render"
    bl_options = {'REGISTER'}

    def execute(self, context):
        if is_active():
            scene = _scene()
            if bpy.app.is_job_running('RENDER'):
                bpy.ops.render.render_stop()
            _stop(scene)
            return {'FINISHED'}

        scene = context.scene
        if scene.camera is None:
            self.report({'ERROR'}, "The scene has no camera to render from")
            return {'CANCELLED'}
        _start(scene)
        return {'FINISHED'}


classes = (
    RENDER_OT_interactive,
)
