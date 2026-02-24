import socket
import threading

from kivy.app import App
from kivy.clock import Clock
from kivy.uix.boxlayout import BoxLayout
from kivy.uix.label import Label
from kivy.core.window import Window


# ================= CONFIG =================

UDP_PORT = 3333       # Must match ESP32
BUFFER_SIZE = 256

# ==========================================


class Dashboard(BoxLayout):

    def __init__(self, **kwargs):
        super().__init__(orientation="vertical", **kwargs)

        self.padding = 20
        self.spacing = 15


        self.raw_lbl = Label(text="Raw Frequency: -- Hz",
                             font_size=28)

        self.filt_lbl = Label(text="Filtered Frequency: -- Hz",
                              font_size=28)

        self.flow_lbl = Label(text="Flow Rate: -- L/min",
                              font_size=28)

        self.tds_lbl = Label(text="TDS: -- ppm",
                             font_size=28)


        self.add_widget(self.raw_lbl)
        self.add_widget(self.filt_lbl)
        self.add_widget(self.flow_lbl)
        self.add_widget(self.tds_lbl)


    def update_values(self, raw, filt, flow, tds):

        self.raw_lbl.text = f"Raw Frequency: {raw:.2f} Hz"
        self.filt_lbl.text = f"Filtered Frequency: {filt:.2f} Hz"
        self.flow_lbl.text = f"Flow Rate: {flow:.3f} L/min"
        self.tds_lbl.text = f"TDS: {tds:.2f} ppm"



class UDPListener(threading.Thread):

    def __init__(self, callback):
        super().__init__(daemon=True)

        self.callback = callback

        self.sock = socket.socket(socket.AF_INET,
                                  socket.SOCK_DGRAM)

        self.sock.bind(("0.0.0.0", UDP_PORT))


    def run(self):

        while True:

            data, _ = self.sock.recvfrom(BUFFER_SIZE)

            try:
                msg = data.decode().strip()

                # Expect: raw,filt,flow,tds
                parts = msg.split(",")

                if len(parts) != 4:
                    continue

                raw = float(parts[0])
                filt = float(parts[1])
                flow = float(parts[2])
                tds = float(parts[3])

                # Send to GUI thread
                Clock.schedule_once(
                    lambda dt:
                    self.callback(raw, filt, flow, tds)
                )

            except:
                pass



class TDSApp(App):

    def build(self):

        Window.size = (600, 400)
        Window.clearcolor = (0.1, 0.1, 0.1, 1)

        self.dashboard = Dashboard()

        listener = UDPListener(
            self.dashboard.update_values)

        listener.start()

        return self.dashboard



if __name__ == "__main__":
    TDSApp().run()