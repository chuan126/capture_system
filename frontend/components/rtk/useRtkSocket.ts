"use client";

import { useEffect, useState } from "react";

import { parseRtkText } from "./rtkProtocol";
import type { RtkSnapshot, RtkStatusMessage } from "./rtkProtocol";

export type RtkConnectionState = {
  connection: "connecting" | "connected" | "reconnecting" | "closed";
  streamState: RtkStatusMessage["state"] | "waiting";
  detail: string;
  snapshot: RtkSnapshot | null;
};

const INITIAL_STATE: RtkConnectionState = {
  connection: "connecting",
  streamState: "waiting",
  detail: "正在连接RTK服务",
  snapshot: null,
};

export function useRtkSocket(): RtkConnectionState {
  const [state, setState] = useState<RtkConnectionState>(INITIAL_STATE);

  useEffect(() => {
    let stopped = false;
    let socket: WebSocket | null = null;
    let reconnectTimer: ReturnType<typeof setTimeout> | null = null;
    let reconnectAttempt = 0;
    let stableTimer: ReturnType<typeof setTimeout> | null = null;

    const scheduleReconnect = () => {
      if (stopped) {
        return;
      }
      const delays = [1_000, 2_000, 4_000, 8_000, 10_000];
      const delay = delays[Math.min(reconnectAttempt, delays.length - 1)];
      reconnectAttempt += 1;
      setState((current) => ({
        ...current,
        connection: "reconnecting",
        streamState: "waiting",
        detail: `RTK连接已断开，${delay / 1_000}秒后重连`,
      }));
      reconnectTimer = setTimeout(connect, delay);
    };

    const connect = () => {
      if (stopped) {
        return;
      }
      const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
      socket = new WebSocket(`${protocol}//${window.location.host}/ws/v1/rtk`);
      setState((current) => ({
        ...current,
        connection: reconnectAttempt === 0 ? "connecting" : "reconnecting",
        detail: reconnectAttempt === 0 ? "正在连接RTK服务" : "正在重新连接RTK服务",
      }));

      socket.onopen = () => {
        setState((current) => ({
          ...current,
          connection: "connected",
          detail: "RTK服务已连接",
        }));
        stableTimer = setTimeout(() => {
          reconnectAttempt = 0;
        }, 10_000);
      };

      socket.onmessage = (event) => {
        try {
          if (typeof event.data !== "string") {
            throw new Error("浏览器收到非文本RTK消息");
          }
          const message = parseRtkText(event.data);
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
              detail: message.serial_message,
              snapshot: message,
            }));
          }
        } catch (error) {
          setState((current) => ({
            ...current,
            streamState: "degraded",
            detail: error instanceof Error ? error.message : "RTK消息解析失败",
          }));
        }
      };

      socket.onerror = () => socket?.close();
      socket.onclose = () => {
        if (stableTimer !== null) {
          clearTimeout(stableTimer);
          stableTimer = null;
        }
        scheduleReconnect();
      };
    };

    connect();
    return () => {
      stopped = true;
      if (reconnectTimer !== null) {
        clearTimeout(reconnectTimer);
      }
      if (stableTimer !== null) {
        clearTimeout(stableTimer);
      }
      if (socket && socket.readyState < WebSocket.CLOSING) {
        socket.close(1000, "RTK组件已卸载");
      }
    };
  }, []);

  return state;
}
