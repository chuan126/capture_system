"use client";

import { useEffect, useState } from "react";

import { parseClearanceText } from "./clearanceProtocol";
import type { ClearanceSnapshot, ClearanceStatusMessage } from "./clearanceProtocol";

export type ClearanceConnectionState = {
  connection: "connecting" | "connected" | "reconnecting" | "closed";
  streamState: ClearanceStatusMessage["state"] | "waiting";
  detail: string;
  snapshot: ClearanceSnapshot | null;
};

const INITIAL_STATE: ClearanceConnectionState = {
  connection: "connecting",
  streamState: "waiting",
  detail: "正在连接净空结果服务",
  snapshot: null,
};

export function useClearanceSocket(): ClearanceConnectionState {
  const [state, setState] = useState<ClearanceConnectionState>(INITIAL_STATE);

  useEffect(() => {
    let stopped = false;
    let socket: WebSocket | null = null;
    let reconnectTimer: ReturnType<typeof setTimeout> | null = null;
    let reconnectAttempt = 0;

    const scheduleReconnect = () => {
      if (stopped) return;
      const delays = [1_000, 2_000, 4_000, 8_000, 10_000];
      const delay = delays[Math.min(reconnectAttempt, delays.length - 1)];
      reconnectAttempt += 1;
      setState((current) => ({
        ...current,
        connection: "reconnecting",
        streamState: "waiting",
        detail: `净空结果连接已断开，${delay / 1_000}秒后重连`,
      }));
      reconnectTimer = setTimeout(connect, delay);
    };

    const connect = () => {
      if (stopped) return;
      const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
      socket = new WebSocket(`${protocol}//${window.location.host}/ws/v1/clearance`);
      setState((current) => ({
        ...current,
        connection: reconnectAttempt === 0 ? "connecting" : "reconnecting",
        detail: reconnectAttempt === 0 ? "正在连接净空结果服务" : "正在重新连接净空结果服务",
      }));

      socket.onopen = () => {
        reconnectAttempt = 0;
        setState((current) => ({ ...current, connection: "connected" }));
      };
      socket.onmessage = (event) => {
        try {
          if (typeof event.data !== "string") throw new Error("浏览器收到非文本净空消息");
          const message = parseClearanceText(event.data);
          if (message.type === "status") {
            setState((current) => ({
              ...current,
              connection: "connected",
              streamState: message.state,
              detail: message.detail,
            }));
          } else {
            setState((current) => ({
              ...current,
              connection: "connected",
              streamState: "streaming",
              detail: message.valid ? "本帧高度有效" : `本帧无效：${message.invalid_reason}`,
              snapshot: message,
            }));
          }
        } catch (error) {
          setState((current) => ({
            ...current,
            streamState: "degraded",
            detail: error instanceof Error ? error.message : "净空消息解析失败",
          }));
        }
      };
      socket.onerror = () => socket?.close();
      socket.onclose = scheduleReconnect;
    };

    connect();
    return () => {
      stopped = true;
      if (reconnectTimer !== null) clearTimeout(reconnectTimer);
      if (socket && socket.readyState < WebSocket.CLOSING) {
        socket.close(1000, "净空组件已卸载");
      }
    };
  }, []);

  return state;
}
