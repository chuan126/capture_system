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

const SNAPSHOT_TIMEOUT_MS = 5000;

export function useSystemStatusSocket(): SystemStatusConnection {
  const [state, setState] = useState<SystemStatusConnection>(INITIAL);
  useEffect(() => {
    let stopped = false;
    let socket: WebSocket | null = null;
    let timer: ReturnType<typeof setTimeout> | null = null;
    let watchdog: ReturnType<typeof setInterval> | null = null;
    let attempt = 0;
    let lastSnapshotAt = 0;

    const markUnavailable = (detail: string) => {
      lastSnapshotAt = 0;
      setState((current) => ({
        ...current,
        streamState: "degraded",
        detail,
        snapshot: null,
      }));
    };

    const reconnect = () => {
      if (stopped) return;
      const delay = [1000, 2000, 4000, 8000, 10000][Math.min(attempt, 4)];
      attempt += 1;
      lastSnapshotAt = 0;
      setState((current) => ({
        ...current,
        connection: "reconnecting",
        streamState: "waiting",
        detail: "系统状态连接已断开",
        snapshot: null,
      }));
      timer = setTimeout(connect, delay);
    };

    const connect = () => {
      if (stopped) return;
      const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
      socket = new WebSocket(`${protocol}//${window.location.host}/ws/v1/system-status`);
      socket.onopen = () => setState((current) => ({
        ...current,
        connection: "connected",
        streamState: "waiting",
        detail: "系统状态服务已连接，等待设备诊断",
        snapshot: null,
      }));
      socket.onmessage = (event) => {
        try {
          if (typeof event.data !== "string") throw new Error("浏览器收到非文本系统状态消息");
          const message = parseSystemStatusText(event.data);
          if (message.type === "status") {
            if (message.state !== "streaming") lastSnapshotAt = 0;
            setState((current) => ({
              ...current,
              connection: "connected",
              streamState: message.state,
              detail: message.detail,
              snapshot: message.state === "streaming" ? current.snapshot : null,
            }));
          } else {
            attempt = 0;
            lastSnapshotAt = Date.now();
            setState((current) => ({
              ...current,
              connection: "connected",
              streamState: "streaming",
              detail: "设备诊断数据实时更新",
              snapshot: message,
            }));
          }
        } catch (error) {
          markUnavailable(error instanceof Error ? error.message : "系统状态解析失败");
        }
      };
      socket.onerror = () => socket?.close();
      socket.onclose = reconnect;
    };

    watchdog = setInterval(() => {
      if (lastSnapshotAt > 0 && Date.now() - lastSnapshotAt > SNAPSHOT_TIMEOUT_MS) {
        markUnavailable("超过 5 秒未收到设备诊断数据");
      }
    }, 1000);

    connect();
    return () => {
      stopped = true;
      if (timer !== null) clearTimeout(timer);
      if (watchdog !== null) clearInterval(watchdog);
      if (socket && socket.readyState < WebSocket.CLOSING) socket.close(1000, "系统状态组件已卸载");
    };
  }, []);
  return state;
}
