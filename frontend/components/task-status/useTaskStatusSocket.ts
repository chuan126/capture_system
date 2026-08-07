"use client";

import { useEffect, useState } from "react";

import { parseTaskStatusSnapshot } from "@/components/task-status/taskStatusProtocol";
import type { TaskStatusSnapshot } from "@/components/task-status/taskStatusProtocol";

export const useTaskStatusSocket = () => {
  const [snapshot, setSnapshot] = useState<TaskStatusSnapshot | null>(null);
  const [connection, setConnection] = useState<"connecting" | "connected" | "disconnected">("connecting");
  const [streamState, setStreamState] = useState("waiting");
  const [detail, setDetail] = useState("正在连接任务状态");

  useEffect(() => {
    let stopped = false;
    let socket: WebSocket | null = null;
    let reconnectTimer: number | null = null;

    const connect = () => {
      if (stopped) return;
      setConnection("connecting");
      const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
      socket = new WebSocket(`${protocol}//${window.location.host}/ws/v1/task-status`);
      socket.onopen = () => {
        if (!stopped) setConnection("connected");
      };
      socket.onmessage = (event) => {
        if (stopped || typeof event.data !== "string") return;
        try {
          const message: unknown = JSON.parse(event.data);
          if (typeof message === "object" && message !== null && "type" in message && message.type === "status") {
            const statusMessage = message as { state?: unknown; detail?: unknown };
            setStreamState(typeof statusMessage.state === "string" ? statusMessage.state : "unknown");
            setDetail(typeof statusMessage.detail === "string" ? statusMessage.detail : "任务状态未知");
            return;
          }
          const parsed = parseTaskStatusSnapshot(message);
          if (parsed) setSnapshot(parsed);
        } catch {
          setDetail("任务状态消息无法解析");
        }
      };
      socket.onclose = () => {
        if (stopped) return;
        setConnection("disconnected");
        setStreamState("disconnected");
        setDetail("任务状态连接已断开，正在重连");
        reconnectTimer = window.setTimeout(connect, 1000);
      };
      socket.onerror = () => socket?.close();
    };

    connect();
    return () => {
      stopped = true;
      if (reconnectTimer !== null) window.clearTimeout(reconnectTimer);
      socket?.close();
    };
  }, []);

  return { snapshot, connection, streamState, detail };
};
