"use client";

import { useEffect, useState } from "react";

import { parseSystemStatusText } from "./systemStatusProtocol";
import type { SystemStatusSnapshot, SystemStreamStatus } from "./systemStatusProtocol";

export type SystemStatusConnection = {
  connection: "connecting" | "connected" | "reconnecting";
  streamState: SystemStreamStatus["state"] | "waiting";
  detail: string;
  snapshot: SystemStatusSnapshot | null;
};

const INITIAL: SystemStatusConnection = {
  connection: "connecting",
  streamState: "waiting",
  detail: "正在连接系统状态服务",
  snapshot: null,
};

export function useSystemStatusSocket(): SystemStatusConnection {
  const [state, setState] = useState<SystemStatusConnection>(INITIAL);
  useEffect(() => {
    let stopped = false;
    let socket: WebSocket | null = null;
    let timer: ReturnType<typeof setTimeout> | null = null;
    let attempt = 0;
    const reconnect = () => {
      if (stopped) return;
      const delay = [1000, 2000, 4000, 8000, 10000][Math.min(attempt, 4)];
      attempt += 1;
      setState((current) => ({...current, connection: "reconnecting", streamState: "waiting", detail: "系统状态连接已断开"}));
      timer = setTimeout(connect, delay);
    };
    const connect = () => {
      if (stopped) return;
      const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
      socket = new WebSocket(`${protocol}//${window.location.host}/ws/v1/system-status`);
      socket.onopen = () => setState((current) => ({...current, connection: "connected", detail: "系统状态服务已连接"}));
      socket.onmessage = (event) => {
        try {
          if (typeof event.data !== "string") throw new Error("浏览器收到非文本系统状态消息");
          const message = parseSystemStatusText(event.data);
          if (message.type === "status") {
            setState((current) => ({...current, connection: "connected", streamState: message.state, detail: message.detail}));
          } else {
            attempt = 0;
            setState((current) => ({...current, connection: "connected", snapshot: message}));
          }
        } catch (error) {
          setState((current) => ({...current, streamState: "degraded", detail: error instanceof Error ? error.message : "系统状态解析失败"}));
        }
      };
      socket.onerror = () => socket?.close();
      socket.onclose = reconnect;
    };
    connect();
    return () => {
      stopped = true;
      if (timer !== null) clearTimeout(timer);
      if (socket && socket.readyState < WebSocket.CLOSING) socket.close(1000, "系统状态组件已卸载");
    };
  }, []);
  return state;
}
