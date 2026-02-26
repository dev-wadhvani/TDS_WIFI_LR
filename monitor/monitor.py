import socket
import threading

from kivy.app import App
from kivy.clock import Clock
from kivy.uix.boxlayout import BoxLayout
from kivy.uix.label import Label
from kivy.core.window import Window


UDP_PORT = 3333
BUFFER_SIZE = 256


class Dashboard(BoxLayout):

    def __init__(self, **kwargs):
        super().__init__(orientation="vertical", **kwargs)

        self.padding = 20
        self.spacing = 15

        self.raw_lbl = Label(text="Raw Frequency: -", font_size=28)
        self.filt_lbl = Label(text="Filtered Frequency: -", font_size=28)
        self.flow_lbl = Label(text="Flow Rate: -", font_size=28)
        self.tds_lbl = Label(text="TDS: -", font_size=28)

        self.add_widget(self.raw_lbl)
        self.add_widget(self.filt_lbl)
        self.add_widget(self.flow_lbl)
        self.add_widget(self.tds_lbl)


    def set_off(self):

        self.raw_lbl.text = "Raw Frequency: -"
        self.filt_lbl.text = "Filtered Frequency: -"
        self.flow_lbl.text = "Flow Rate: -"
        self.tds_lbl.text = "TDS: -"


    def set_warmup(self):

        self.raw_lbl.text = "Raw Frequency: warmup"
        self.filt_lbl.text = "Filtered Frequency: warmup"
        self.flow_lbl.text = "Flow Rate: warmup"
        self.tds_lbl.text = "TDS: warmup"


    def set_data(self, raw, filt, flow, tds):

        self.raw_lbl.text = f"Raw Frequency: {raw:.2f}"
        self.filt_lbl.text = f"Filtered Frequency: {filt:.2f}"
        self.flow_lbl.text = f"Flow Rate: {flow:.3f}"
        self.tds_lbl.text = f"TDS: {tds:.2f}"


class UDPListener(threading.Thread):

    def __init__(self, dashboard):
        super().__init__(daemon=True)

        self.dashboard = dashboard

        self.sock = socket.socket(socket.AF_INET,
                                  socket.SOCK_DGRAM)

        self.sock.bind(("0.0.0.0", UDP_PORT))


    def run(self):

        while True:

            data, _ = self.sock.recvfrom(BUFFER_SIZE)

            try:
                msg = data.decode().strip()


                if msg == "-,-,-,-":

                    Clock.schedule_once(
                        lambda dt: self.dashboard.set_off())
                    continue


                if msg == "warmup,warmup,warmup,warmup":

                    Clock.schedule_once(
                        lambda dt: self.dashboard.set_warmup())
                    continue


                parts = msg.split(",")

                if len(parts) != 4:
                    continue


                raw = float(parts[0])
                filt = float(parts[1])
                flow = float(parts[2])
                tds = float(parts[3])


                Clock.schedule_once(
                    lambda dt:
                    self.dashboard.set_data(
                        raw, filt, flow, tds)
                )

            except:
                pass



class TDSApp(App):

    def build(self):

        Window.size = (600, 400)
        Window.clearcolor = (0.1, 0.1, 0.1, 1)

        dash = Dashboard()

        listener = UDPListener(dash)
        listener.start()

        return dash



if __name__ == "__main__":
    TDSApp().run()