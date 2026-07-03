"""
Blazepod GUI — Windows Python 3
pip install pyserial
python blazepod_gui.py

UART message formats observed from nRF printk:
  Game start M1 : "Mode 1 GO (R=FF G=00 B=00)"
  Game start M2 : "Mode 2 GO"
  Stopped       : "Game STOPPED"
  Round start   : "M1:pod=2"  /  "M2 set: odd_pod=1 count=2"
  Pod1 correct  : "POD1:RT:5224"
  Pod2-4 correct: "Pod notify:[POD2:RT:4800]"
  Pod1 timeout  : "POD1:TO"   (printed directly)
  Pod2-4 timeout: "Pod notify:[POD2:TIMEOUT]"
  Mode switch   : "MODE 1" / "MODE 2"
  Wrong (M2)    : "WRONG pod..."  or printed via pod_notify
"""

import tkinter as tk
from tkinter import ttk
import serial
import serial.tools.list_ports
import threading
import re
import time

COLOURS = ["RED", "GREEN", "BLUE", "YELLOW", "PINK", "VIOLET"]
COLOUR_HEX = {
    "RED":    "#ff3333",
    "GREEN":  "#00cc55",
    "BLUE":   "#4488ff",
    "YELLOW": "#ffdd00",
    "PINK":   "#ff1493",
    "VIOLET": "#9400d3",
}

BG     = "#0e0e0e"
SURF   = "#1a1a1a"
SURF2  = "#222222"
BORDER = "#2e2e2e"
TEXT   = "#e8e8e8"
MUTED  = "#606060"
GREEN  = "#00d97e"
RED    = "#ff4444"
BLUE   = "#4f8ef7"
WHITE  = "#ffffff"


class BlazeApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Blazepod Control")
        self.geometry("480x600")
        self.resizable(False, False)
        self.configure(bg=BG)

        self.ser        = None
        self.connected  = False
        self.mode       = 1
        self.running    = False
        self.sel_colour = "RED"

        self.stats = {
            1: {"total": 0, "correct": 0},
            2: {"total": 0, "correct": 0},
        }

        self._build_ui()
        self._refresh_ports()
        self._render()

    # ─────────────────────────────────────────────────────────
    # UI  (pack only)
    # ─────────────────────────────────────────────────────────
    def _build_ui(self):
        # title
        hdr = tk.Frame(self, bg=BG)
        hdr.pack(fill="x", padx=18, pady=(16, 6))
        tk.Label(hdr, text="Blaze", bg=BG, fg=GREEN,
                 font=("Segoe UI", 20, "bold")).pack(side="left")
        tk.Label(hdr, text="pod Control", bg=BG, fg=WHITE,
                 font=("Segoe UI", 20, "bold")).pack(side="left")
        self.status_lbl = tk.Label(hdr, text="● idle",
                                   bg=BG, fg=MUTED,
                                   font=("Segoe UI", 11))
        self.status_lbl.pack(side="right")

        # serial row
        sr = tk.Frame(self, bg=SURF, bd=0,
                      highlightthickness=1, highlightbackground=BORDER)
        sr.pack(fill="x", padx=18, pady=4)
        tk.Label(sr, text="Port", bg=SURF, fg=MUTED,
                 font=("Segoe UI", 10)).pack(side="left", padx=(12,4), pady=10)
        self.port_var = tk.StringVar()
        self.port_cb  = ttk.Combobox(sr, textvariable=self.port_var,
                                     width=9, state="readonly")
        self.port_cb.pack(side="left", padx=4)
        tk.Button(sr, text="⟳", bg=SURF, fg=MUTED,
                  activebackground=SURF2, activeforeground=TEXT,
                  font=("Segoe UI", 12), relief="flat", bd=0,
                  padx=4, cursor="hand2",
                  command=self._refresh_ports).pack(side="left", padx=2)
        self.conn_btn = tk.Button(sr, text="Connect",
                                  bg=SURF2, fg=GREEN,
                                  activebackground=BORDER, activeforeground=GREEN,
                                  font=("Segoe UI", 10, "bold"),
                                  relief="flat", bd=0, padx=14, pady=6,
                                  cursor="hand2", command=self._toggle_connect)
        self.conn_btn.pack(side="right", padx=10, pady=8)

        # mode
        mc = tk.Frame(self, bg=SURF, bd=0,
                      highlightthickness=1, highlightbackground=BORDER)
        mc.pack(fill="x", padx=18, pady=4)
        tk.Label(mc, text="MODE", bg=SURF, fg=MUTED,
                 font=("Segoe UI", 9)).pack(side="left", padx=(12,6), pady=10)
        self.mode_lbl = tk.Label(mc, text="Mode 1 — Reaction",
                                 bg=SURF, fg=GREEN,
                                 font=("Segoe UI", 12, "bold"))
        self.mode_lbl.pack(side="left")
        tk.Label(mc, text="(hardware switch)", bg=SURF, fg=MUTED,
                 font=("Segoe UI", 9)).pack(side="left", padx=8)

        # score cards
        score_row = tk.Frame(self, bg=BG)
        score_row.pack(fill="x", padx=18, pady=6)
        self.score_cards = {}
        self.score_lbls  = {}
        for idx, (m, title) in enumerate([(1, "MODE 1"), (2, "MODE 2")]):
            f = tk.Frame(score_row, bg=SURF, bd=0,
                         highlightthickness=2,
                         highlightbackground=BORDER)
            f.pack(side="left", fill="both", expand=True,
                   padx=(0 if idx == 0 else 6, 0))
            tk.Label(f, text=title, bg=SURF, fg=MUTED,
                     font=("Segoe UI", 9)).pack(pady=(12, 2))
            lbl = tk.Label(f, text="0 / 0", bg=SURF, fg=WHITE,
                           font=("Segoe UI", 32, "bold"))
            lbl.pack()
            tk.Label(f, text="correct / total", bg=SURF, fg=MUTED,
                     font=("Segoe UI", 9)).pack(pady=(2, 12))
            self.score_cards[m] = f
            self.score_lbls[m]  = lbl

        # start / stop
        ctrl = tk.Frame(self, bg=BG)
        ctrl.pack(fill="x", padx=18, pady=8)
        self.start_btn = tk.Button(ctrl, text="▶  START",
                                   bg="#003322", fg=GREEN,
                                   activebackground="#004433",
                                   activeforeground=GREEN,
                                   font=("Segoe UI", 15, "bold"),
                                   relief="flat", bd=0, pady=16,
                                   cursor="hand2", command=self._do_start)
        self.start_btn.pack(side="left", fill="x", expand=True, padx=(0,6))
        self.stop_btn = tk.Button(ctrl, text="■  STOP",
                                  bg="#330000", fg=RED,
                                  activebackground="#440000",
                                  activeforeground=RED,
                                  font=("Segoe UI", 15, "bold"),
                                  relief="flat", bd=0, pady=16,
                                  cursor="hand2", command=self._do_stop)
        self.stop_btn.pack(side="left", fill="x", expand=True, padx=(6,0))

        # colour picker
        cpick = tk.Frame(self, bg=BG)
        cpick.pack(fill="x", padx=18, pady=(0,4))
        tk.Label(cpick, text="COLOUR  (Mode 1 only)",
                 bg=BG, fg=MUTED,
                 font=("Segoe UI", 9)).pack(anchor="w", pady=(4,6))
        crow = tk.Frame(cpick, bg=BG)
        crow.pack(anchor="w")
        self.colour_btns = {}
        for c in COLOURS:
            fg = "#000" if c in ("YELLOW","GREEN") else "#fff"
            b  = tk.Button(crow, text=c,
                           bg=COLOUR_HEX[c], fg=fg,
                           activebackground=COLOUR_HEX[c], activeforeground=fg,
                           font=("Segoe UI", 9, "bold"),
                           relief="flat", bd=0, padx=9, pady=7,
                           cursor="hand2",
                           command=lambda col=c: self._pick_colour(col))
            b.pack(side="left", padx=2)
            self.colour_btns[c] = b

        # timeout
        tf = tk.Frame(self, bg=BG)
        tf.pack(fill="x", padx=18, pady=(0,6))
        tk.Label(tf, text="TIMEOUT", bg=BG, fg=MUTED,
                 font=("Segoe UI", 9)).pack(anchor="w", pady=(0,4))
        trow = tk.Frame(tf, bg=BG)
        trow.pack(fill="x")
        tk.Label(trow, text="1s", bg=BG, fg=MUTED,
                 font=("Segoe UI", 9)).pack(side="left")
        self.to_secs = 10
        self.to_lbl  = tk.Label(trow, text="10 s", bg=BG, fg=WHITE,
                                font=("Segoe UI", 11, "bold"), width=5)

        def _on_slider(v):
            self.to_secs = int(float(v))
            self.to_lbl.config(text=f"{self.to_secs} s")

        tk.Scale(trow, from_=1, to=60, orient="horizontal",
                 bg=BG, fg=TEXT, troughcolor=SURF2,
                 highlightthickness=0, activebackground=GREEN,
                 showvalue=False, command=_on_slider
                 ).pack(side="left", fill="x", expand=True, padx=4)
        tk.Label(trow, text="60s", bg=BG, fg=MUTED,
                 font=("Segoe UI", 9)).pack(side="left")
        self.to_lbl.pack(side="left", padx=6)
        tk.Button(trow, text="Set", bg=SURF2, fg=TEXT,
                  activebackground=BORDER, activeforeground=WHITE,
                  font=("Segoe UI", 9), relief="flat", bd=0,
                  padx=10, pady=4, cursor="hand2",
                  command=self._send_timeout).pack(side="left")

        # reset / clear
        bot = tk.Frame(self, bg=BG)
        bot.pack(fill="x", padx=18, pady=(0,4))
        tk.Button(bot, text="↺ Reset stats", bg=BG, fg=MUTED,
                  activebackground=SURF, activeforeground=TEXT,
                  font=("Segoe UI", 9), relief="flat", bd=0,
                  padx=6, pady=2, cursor="hand2",
                  command=self._reset_stats).pack(side="left")
        tk.Button(bot, text="Clear log", bg=BG, fg=MUTED,
                  activebackground=SURF, activeforeground=TEXT,
                  font=("Segoe UI", 9), relief="flat", bd=0,
                  padx=6, pady=2, cursor="hand2",
                  command=self._clear_log).pack(side="right")

        # log
        self.log_box = tk.Text(self, bg=SURF, fg=TEXT,
                               font=("Consolas", 9),
                               relief="flat", bd=0,
                               state="disabled", wrap="word", height=8)
        self.log_box.pack(fill="both", expand=True, padx=18, pady=(0,18))
        self.log_box.tag_config("rx",  foreground=TEXT)
        self.log_box.tag_config("tx",  foreground=GREEN)
        self.log_box.tag_config("sys", foreground=MUTED)
        self.log_box.tag_config("ok",  foreground=GREEN)
        self.log_box.tag_config("err", foreground=RED)

    # ─────────────────────────────────────────────────────────
    # SERIAL
    # ─────────────────────────────────────────────────────────
    def _refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.port_cb["values"] = ports
        if ports:
            self.port_var.set(ports[0])
        self._log("sys", f"Ports: {ports or ['none found']}")

    def _toggle_connect(self):
        if self.connected:
            self._disconnect()
        else:
            self._connect()

    def _connect(self):
        port = self.port_var.get()
        if not port:
            self._log("err", "No port selected")
            return
        try:
            self.ser = serial.Serial(port, 115200, timeout=0.1)
            self.connected = True
            self._log("sys", f"Connected {port} @ 115200")
            threading.Thread(target=self._read_loop, daemon=True).start()
            self._render()
        except Exception as e:
            self._log("err", f"Connect failed: {e}")

    def _disconnect(self):
        self.connected = False
        self.running   = False
        try:
            if self.ser:
                self.ser.close()
        except Exception:
            pass
        self.ser = None
        self._log("sys", "Disconnected")
        self._render()

    # ─────────────────────────────────────────────────────────
    # READ LOOP
    # ─────────────────────────────────────────────────────────
    def _read_loop(self):
        buf = ""
        while self.connected and self.ser:
            try:
                raw = self.ser.read(256)
                if not raw:
                    continue
                buf += raw.decode("utf-8", errors="replace")
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    line = line.strip()
                    if line:
                        self.after(0, self._handle_rx, line)
            except Exception as e:
                if self.connected:
                    self.after(0, self._log, "err", f"Read error: {e}")
                break

    # ─────────────────────────────────────────────────────────
    # PARSE  — matched against real UART strings from PuTTY log
    #
    # From printk() in main.c these come over UART:
    #
    #  Game start M1 : "Mode 1 GO (R=FF G=00 B=00)"
    #  Game start M2 : "Mode 2 GO"
    #  Game stopped  : "Game STOPPED"
    #  Mode switch   : "MODE 1"  /  "MODE 2"
    #
    #  Round start M1: "M1:pod=1"  "M1:pod=2"  etc.
    #  Round start M2: "M2 set: odd_pod=1 count=2"
    #
    #  Pod1 RT       : "POD1:RT:5224"
    #  Pod2-4 RT     : "Pod notify:[POD2:RT:4800]"
    #
    #  Pod1 timeout  : "POD1:TO"
    #  Pod2-4 timeout: "Pod notify:[POD2:TIMEOUT]"
    #
    #  Pod2-4 wrong  : "Pod notify:[POD2:WRONG]"   (Mode 2)
    #  Pod1 wrong    : "POD1:WRONG"                (Mode 2)
    # ─────────────────────────────────────────────────────────
    def _handle_rx(self, line):
        self._log("rx", f"← {line}")

        # ── game started ──────────────────────────────────────
        # "Mode 1 GO (R=FF G=00 B=00)"   or   "Mode 2 GO"
        if re.search(r"Mode [12] GO", line, re.IGNORECASE):
            m = 2 if "2" in line.split("GO")[0] else 1
            self.mode    = m
            self.running = True
            self._set_status("running…", GREEN)
            self._render()
            return

        # ── game stopped ──────────────────────────────────────
        # "Game STOPPED"
        if "STOPPED" in line.upper():
            self.running = False
            self._set_status("idle", MUTED)
            self._render()
            return

        # ── mode switch (hardware button) ─────────────────────
        # "MODE 1"  /  "MODE 2"
        if re.fullmatch(r"MODE [12]", line.strip(), re.IGNORECASE):
            self.mode = int(line.strip()[-1])
            self._log("sys", f"Mode → {self.mode}")
            self._render()
            return

        # ── round started Mode 1 ──────────────────────────────
        # "M1:pod=2"
        if re.match(r"M1:pod=\d", line, re.IGNORECASE):
            self.stats[1]["total"] += 1
            self._update_score()
            return

        # ── round started Mode 2 ──────────────────────────────
        # "M2 set: odd_pod=1 count=2"
        if re.match(r"M2 set:", line, re.IGNORECASE):
            self.stats[2]["total"] += 1
            self._update_score()
            return

        # ── correct tap — Pod1 ────────────────────────────────
        # "POD1:RT:5224"
        m1 = re.fullmatch(r"POD1:RT:(\d+)", line, re.IGNORECASE)
        if m1:
            ms = int(m1.group(1))
            self.stats[self.mode]["correct"] += 1
            self._log("ok", f"✓ POD1  {ms} ms")
            self._set_status(f"correct ✓  {ms} ms", GREEN)
            self._flash(self.mode, ok=True)
            self._update_score()
            return

        # ── correct tap — Pod2/3/4 ────────────────────────────
        # "Pod notify:[POD2:RT:4800]"
        m2 = re.search(r"Pod notify:\[POD(\d):RT:(\d+)\]", line, re.IGNORECASE)
        if m2:
            pod = m2.group(1)
            ms  = int(m2.group(2))
            self.stats[self.mode]["correct"] += 1
            self._log("ok", f"✓ POD{pod}  {ms} ms")
            self._set_status(f"correct ✓  {ms} ms", GREEN)
            self._flash(self.mode, ok=True)
            self._update_score()
            return

        # ── timeout / wrong — just log, no stat change needed ─
        # (miss = total - correct, no separate counter)
        if re.search(r"POD\d:(TO|TIMEOUT|WRONG)", line, re.IGNORECASE):
            self._log("err", f"✗ {line}")
            self._set_status("miss", RED)
            return
        if re.search(r"Pod notify:\[POD\d:(TIMEOUT|WRONG)\]", line, re.IGNORECASE):
            self._log("err", f"✗ {line}")
            self._set_status("miss", RED)
            return
        if "ROUND:TO" in line.upper():
            self._log("err", "✗ Round timeout")
            self._set_status("timeout", RED)
            return

    # ─────────────────────────────────────────────────────────
    # SEND
    # ─────────────────────────────────────────────────────────
    def _send(self, cmd):
        if not self.connected or not self.ser:
            self._log("err", "Not connected")
            return
        try:
            self.ser.write((cmd + "\n").encode("utf-8"))
            self._log("tx", f"→ {cmd}")
        except Exception as e:
            self._log("err", f"Send failed: {e}")

    def _do_start(self):
        # Mode 1: send the selected colour name (e.g. "RED")
        # C code receives colour name → sets colour → starts game
        # Mode 2: send "START"
        cmd = self.sel_colour if self.mode == 1 else "START"
        self._send(cmd)
        self.running = True
        self._render()

    def _do_stop(self):
        self._send("STOP")
        self.running = False
        self._set_status("idle", MUTED)
        self._render()

    def _pick_colour(self, col):
        # Only update selection locally — do NOT send anything.
        # The colour is sent when START is clicked.
        self.sel_colour = col
        self._render()

    def _send_timeout(self):
        # C code expects:  TIMEOUT:10
        secs = self.to_secs
        self._send(f"TIMEOUT:{secs}")

    # ─────────────────────────────────────────────────────────
    # SCORE
    # ─────────────────────────────────────────────────────────
    def _update_score(self):
        for m in (1, 2):
            c = self.stats[m]["correct"]
            t = self.stats[m]["total"]
            self.score_lbls[m].config(text=f"{c} / {t}")

    def _flash(self, mode, ok):
        f      = self.score_cards[mode]
        colour = "#003320" if ok else "#330000"
        border = GREEN    if ok else RED
        f.config(bg=colour, highlightbackground=border)
        for w in f.winfo_children():
            w.config(bg=colour)
        self.after(600, lambda: self._unflash(mode))

    def _unflash(self, mode):
        f = self.score_cards[mode]
        f.config(bg=SURF,
                 highlightbackground=GREEN if mode == self.mode else BORDER)
        for w in f.winfo_children():
            w.config(bg=SURF)

    def _reset_stats(self):
        self.stats = {1: {"total": 0, "correct": 0},
                      2: {"total": 0, "correct": 0}}
        self._update_score()
        self._log("sys", "Stats reset")

    # ─────────────────────────────────────────────────────────
    # RENDER
    # ─────────────────────────────────────────────────────────
    def _render(self):
        self.conn_btn.config(
            text="Disconnect" if self.connected else "Connect",
            fg=RED if self.connected else GREEN)

        self.mode_lbl.config(
            text="Mode 1 — Reaction" if self.mode == 1
                 else "Mode 2 — Odd-one-out",
            fg=GREEN if self.mode == 1 else BLUE)

        for m in (1, 2):
            self.score_cards[m].config(
                highlightbackground=GREEN if m == self.mode else BORDER)

        can_start = self.connected and not self.running
        can_stop  = self.connected and self.running

        self.start_btn.config(
            state="normal" if can_start else "disabled",
            bg="#003322"   if can_start else SURF2)
        self.stop_btn.config(
            state="normal" if can_stop  else "disabled",
            bg="#330000"   if can_stop  else SURF2)

        can_pick = can_start and self.mode == 1
        for name, btn in self.colour_btns.items():
            btn.config(
                state="normal" if can_pick else "disabled",
                relief="sunken" if name == self.sel_colour else "flat",
                bd=3 if name == self.sel_colour else 0)

        self._update_score()

    # ─────────────────────────────────────────────────────────
    # STATUS / LOG
    # ─────────────────────────────────────────────────────────
    def _set_status(self, text, colour=MUTED):
        self.status_lbl.config(text=f"● {text}", fg=colour)

    def _log(self, tag, msg):
        ts = time.strftime("%H:%M:%S")
        self.log_box.config(state="normal")
        self.log_box.insert("end", f"[{ts}]  {msg}\n", tag)
        self.log_box.see("end")
        self.log_box.config(state="disabled")

    def _clear_log(self):
        self.log_box.config(state="normal")
        self.log_box.delete("1.0", "end")
        self.log_box.config(state="disabled")


if __name__ == "__main__":
    BlazeApp().mainloop()
