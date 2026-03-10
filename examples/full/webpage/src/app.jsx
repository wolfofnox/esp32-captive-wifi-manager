import './app.css'
import Router from "preact-router";
import Nav from './utilities/nav.jsx';
import MessageDisplay, { message } from './utilities/message.jsx';
import Status from './utilities/status.jsx';
import Root from './pages/root.jsx';
import Control from './pages/control.jsx';
import WebSocket from './pages/websocket.jsx';

export function App() {

  message('success', 'App loaded', 2000);

  return (
    <div id="content">
      <Nav />
      <Router>
        <Root path="/" />
        <Control path="/control" />
        <WebSocket path="/web-socket" />
      </Router>
      <footer>
        <MessageDisplay />
        <Status />
      </footer>
    </div>
  )
}