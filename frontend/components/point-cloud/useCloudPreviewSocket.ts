"use client";

import { useEffect, useRef, useState } from "react";

import {
  PCV1_MAX_POINTS,
  parseCloudPreviewBinary,
  parseCloudPreviewText,
} from "./cloudPreviewProtocol";
import type {
  CloudPreviewFrame,
  CloudStatusMessage,
  CloudStreamInfo,
} from "./cloudPreviewProtocol";

export type CloudConnectionState = {
  connection: "connecting" | "connected" | "reconnecting" | "closed";
  streamState: CloudStatusMessage["state"] | "waiting";
  detail: string;
  frameId: string;
  pointCount: number;
  receiveFps: number;
  droppedFrames: number;
};

const INITIAL_STATE: CloudConnectionState = {
  connection: "connecting",
  streamState: "waiting",
  detail: "正在连接点云服务",
  frameId: "--",
  pointCount: 0,
  receiveFps: 0,
  droppedFrames: 0,
};

export function useCloudPreviewSocket(
  onFrame: (frame: CloudPreviewFrame) => void,
  socketPath = "/ws/v1/cloud-preview",
): CloudConnectionState {
  const [state, setState] = useState<CloudConnectionState>(INITIAL_STATE);
  const onFrameRef = useRef(onFrame);

  useEffect(() => {
    onFrameRef.current = onFrame;
  }, [onFrame]);

  useEffect(() => {
    let stopped = false;
    let socket: WebSocket | null = null;
    let reconnectTimer: ReturnType<typeof setTimeout> | null = null;
    let reconnectAttempt = 0;
    let stableTimer: ReturnType<typeof setTimeout> | null = null;
    let streamInfo: CloudStreamInfo | null = null;
    let previousSequence: number | null = null;
    let droppedFrames = 0;
    let fpsWindowStarted = performance.now();
    let fpsWindowFrames = 0;
    let receiveFps = 0;

    const scheduleReconnect = () => {
      if (stopped) {
        return;
      }
      const delays = [1_000, 2_000, 4_000, 8_000, 10_000];
      const baseDelay = delays[Math.min(reconnectAttempt, delays.length - 1)];
      const jitter = baseDelay * (Math.random() * 0.2 - 0.1);
      reconnectAttempt += 1;
      setState((current) => ({
        ...current,
        connection: "reconnecting",
        streamState: "waiting",
        detail: `点云连接已断开，${Math.round((baseDelay + jitter) / 1000)}秒后重连`,
      }));
      reconnectTimer = setTimeout(connect, baseDelay + jitter);
    };

    const connect = () => {
      if (stopped) {
        return;
      }

      const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
      socket = new WebSocket(
        `${protocol}//${window.location.host}${socketPath}`,
      );
      socket.binaryType = "arraybuffer";

      setState((current) => ({
        ...current,
        connection: reconnectAttempt === 0 ? "connecting" : "reconnecting",
        detail: reconnectAttempt === 0 ? "正在连接点云服务" : "正在重新连接点云服务",
      }));

      socket.onopen = () => {
        setState((current) => ({
          ...current,
          connection: "connected",
          streamState: "waiting",
          detail: "点云预览服务已连接",
        }));
        stableTimer = setTimeout(() => {
          reconnectAttempt = 0;
        }, 10_000);
      };

      socket.onmessage = (event) => {
        try {
          if (typeof event.data === "string") {
            const message = parseCloudPreviewText(event.data);
            if (message.type === "stream_info") {
              streamInfo = message;
              setState((current) => ({
                ...current,
                frameId: message.frame_id,
              }));
            } else {
              setState((current) => ({
                ...current,
                streamState: message.state,
                detail: message.detail,
              }));
            }
            return;
          }

          if (!(event.data instanceof ArrayBuffer)) {
            throw new Error("浏览器收到非ArrayBuffer点云帧");
          }

          const frame = parseCloudPreviewBinary(
            event.data,
            streamInfo?.max_points ?? PCV1_MAX_POINTS,
          );
          if (previousSequence !== null) {
            const difference = (frame.sequence - previousSequence) >>> 0;
            if (difference > 1 && difference < 0x80000000) {
              droppedFrames += difference - 1;
            }
          }
          previousSequence = frame.sequence;

          fpsWindowFrames += 1;
          const now = performance.now();
          const elapsed = now - fpsWindowStarted;
          if (elapsed >= 1_000) {
            receiveFps = fpsWindowFrames * 1_000 / elapsed;
            fpsWindowFrames = 0;
            fpsWindowStarted = now;
          }

          onFrameRef.current(frame);
          setState((current) => ({
            ...current,
            connection: "connected",
            streamState: "streaming",
            detail: "点云预览正常",
            pointCount: frame.pointCount,
            receiveFps,
            droppedFrames,
          }));
        } catch (error) {
          setState((current) => ({
            ...current,
            streamState: "degraded",
            detail: error instanceof Error ? error.message : "点云帧解析失败",
          }));
        }
      };

      socket.onerror = () => {
        socket?.close();
      };

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
        socket.close(1000, "点云组件已卸载");
      }
    };
  }, [socketPath]);

  return state;
}
