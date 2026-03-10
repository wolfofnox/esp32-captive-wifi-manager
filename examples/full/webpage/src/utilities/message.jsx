import { clear } from 'localforage';
import './message.css';
import { useState, useEffect } from 'preact/hooks';

// Module-scoped queue and runners. Component will assign `runQueue` on mount.
let messageQueue = [];
let messageIsActive = false;
let messageTimeout = null;
let messageStartTime = 0;
const fastMessageDuration = 1000;

// Runner set by the mounted component so `message()` can trigger processing.
let runQueue = null;

export function message(type, messageText, duration) {
  if (type === 'none') {
    // Clear current display immediately and let the mounted runner handle re-processing.
    messageIsActive = false;
    if (runQueue) runQueue();
    return;
  }

  messageQueue.push({ type, message: messageText, duration });
  if (messageIsActive) {
    const elapsed = Date.now() - messageStartTime;
    if (elapsed < fastMessageDuration) {
      // If the current message has been displayed for less than the fast duration, shorten it to make way for the new message.
      clearTimeout(messageTimeout);
      messageTimeout = setTimeout(() => {
        messageIsActive = false;
        if (runQueue) runQueue();
      }, fastMessageDuration - elapsed);
    }
    else {
      // Otherwise, end the current message immediately to show the new one.
      clearTimeout(messageTimeout);
      messageIsActive = false;
      if (runQueue) runQueue();
    }
  }
  if (runQueue) runQueue();
}

export default function MessageDisplay() {
  const [messageState, setMessageState] = useState({ type: '', message: '', startTime: 0 });

  useEffect(() => {
    // Define the runner which processes the queue using component state setters.
    runQueue = () => {
      // If a message is currently displayed, don't start another immediately.
      if (messageIsActive) return;

      if (messageQueue.length === 0) {
        setMessageState({ type: '', message: '', startTime: 0 });
        return;
      }

      messageIsActive = true;
      const { type, message, duration } = messageQueue.shift();
      setMessageState({ type, message, startTime: Date.now() });
      messageStartTime = Date.now();

      const showTime = messageQueue.length > 0 ? fastMessageDuration : (duration || 2000);
      clearTimeout(messageTimeout);
      console.log(`Displaying message: ${message} (type: ${type}) for ${showTime}ms`);

      messageTimeout = setTimeout(() => {
        setMessageState({ type: '', message: '', startTime: 0 });
        messageIsActive = false;
        // If there are more messages, process them immediately.
        if (messageQueue.length > 0) runQueue();
      }, showTime);
    };

    // Process any queued messages on mount.
    runQueue();

    return () => {
      // Cleanup runner and timers on unmount.
      runQueue = null;
      clearTimeout(messageTimeout);
    };
  }, []);

  return (
    <>
      {messageState.type && (
        <div class={`message ${messageState.type}`}>
          {messageState.message}
        </div>
      )}
    </>
  );
}