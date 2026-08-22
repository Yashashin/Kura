"""Kura editor startup.

Menu registration lives in C++ (FKuraModule): Tools -> Kura Library plus the
Content Browser "Send to Kura" context entry (2026-07-12; the earlier Python
Tools-menu entries were removed as duplicates). This file runs at editor
startup because it sits in an enabled plugin's Content/Python folder - the
kura.* modules themselves are imported lazily by the C++ bridge.
"""
