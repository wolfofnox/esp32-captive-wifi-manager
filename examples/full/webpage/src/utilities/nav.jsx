import "./nav.css";

export default function Nav() {
    return (
        <nav>
        <a href="/">Home</a>
        <a href="/control">HTML Post control</a>
        <a href="/web-socket">Web Socket</a>
        <a href="/captive">Captive Portal</a> {/* Captive Portal is accessible even when the device is not connected to a Wi-Fi network */}
        <a href="/restart">Restart</a> {/* Restart the device */}
        {/* Add more links as needed */}
      </nav>
    )
}