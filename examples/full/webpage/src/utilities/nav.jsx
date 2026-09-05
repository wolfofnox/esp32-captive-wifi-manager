import "./nav.css";

export default function Nav() {
    return (
        <nav>
        <a href="/">Home</a>
        <a href="/control">HTML Post control</a>
        <a href="/web-socket">Web Socket</a>
        <a href="/captive">Captive Portal</a> {/* Captive Portal is accessible even when the device is not connected to a Wi-Fi network */}
        <a href="#"
          onClick={(event) => {
            event.preventDefault();

            fetch("/restart", { method: "POST" })
              .then((response) => console.log(response))
              .then(() => window.location.reload());
          }}
        >Restart Device</a> {/* Restart the device */}
        {/* Add more links as needed */}
      </nav>
    )
}