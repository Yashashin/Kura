"""Progress reporting for long operations (2026-08-02).

Every Kura operation that can run long is one blocking Python call from the
Slate panel, so a C++-side slow task could only show a frozen window: there is
no tick point on the C++ side. The reporting therefore lives here, inside the
per-item loops, where `unreal.ScopedSlowTask` gets ticked for real and the
dialog repaints with "137 / 4210 - <asset>".

Editor-side module: it imports unreal and NOTHING from kura, so kura.ingest
and friends can import it freely. The unreal-free data layer (manage / pack /
store) never imports it - those functions take a `progress=None` callable and
kura.editor_api hands them a Reporter.

    with progress.Reporter("Verifying library") as report:
        result = manage.rescan(progress=report)     # report(done, total, label)
"""
import unreal

# Only operations that outlast this ever show a window, so quick ones (a
# handful of assets) stay silent instead of flashing a dialog.
DIALOG_DELAY_SECONDS = 0.5


def is_headless():
    """Commandlet / unattended runs: Slate-adjacent calls are off-limits and
    crash past Python's reach - a slow-task dialog is one of them."""
    cmdline = str(unreal.SystemLibrary.get_command_line()).lower()
    return "-run=" in cmdline or "-unattended" in cmdline


class Reporter:
    """Ticking progress dialog for a loop of known length; a no-op when
    headless. Call it once per item: report(done, total, label).

    The underlying ScopedSlowTask is created on the FIRST call, because the
    total is not always known up front (rescan counts files as it walks). No
    cancel button: ingest/import commit files in two phases and a mid-flight
    abort has no safe meaning yet (SPEC 11, 2026-08-02).
    """

    def __init__(self, title):
        self._title = str(title)
        self._task = None
        self._enabled = not is_headless()

    def __enter__(self):
        return self

    def __exit__(self, *_exc):
        self.close()
        return False

    def __call__(self, done, total, label=""):
        if not self._enabled:
            return
        if self._task is None:
            self._task = unreal.ScopedSlowTask(float(max(int(total), 1)),
                                               self._title)
            self._task.__enter__()
            self._task.make_dialog_delayed(DIALOG_DELAY_SECONDS)
        self._task.enter_progress_frame(1.0, str(label) if label else self._title)

    def close(self):
        if self._task is not None:
            # FSlowTask is only unwound by its scope exit; deleting it without
            # one leaves the editor's slow-task stack unbalanced.
            self._task.__exit__(None, None, None)
            self._task = None

    def __del__(self):
        try:  # safety net for a caller that forgot the `with`
            self.close()
        except Exception:  # noqa: BLE001 - interpreter teardown
            pass


def report_to(progress, done, total, label=""):
    """Call an optional progress callable - the shape the data layer uses so
    it never has to care whether one was passed."""
    if progress is not None:
        progress(done, total, label)
