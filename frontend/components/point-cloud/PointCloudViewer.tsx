"use client";

import { useCallback, useEffect, useRef, useState } from "react";
import * as THREE from "three";
import { OrbitControls } from "three/examples/jsm/controls/OrbitControls.js";

import { PCV1_MAX_POINTS } from "./cloudPreviewProtocol";
import type { CloudPreviewFrame } from "./cloudPreviewProtocol";
import { useCloudPreviewSocket } from "./useCloudPreviewSocket";

export type PointCloudViewMode = "3d" | "top";

type ViewerResources = {
  renderer: THREE.WebGLRenderer;
  scene: THREE.Scene;
  perspectiveCamera: THREE.PerspectiveCamera;
  topCamera: THREE.OrthographicCamera;
  controls: OrbitControls;
  geometry: THREE.BufferGeometry;
  positionAttribute: THREE.BufferAttribute;
  requestRender: () => void;
  fitView: (mode: PointCloudViewMode) => void;
  resize: () => void;
};

type PointCloudViewerProps = {
  viewMode: PointCloudViewMode;
};

export default function PointCloudViewer({
  viewMode,
}: PointCloudViewerProps) {
  const hostRef = useRef<HTMLDivElement>(null);
  const resourcesRef = useRef<ViewerResources | null>(null);
  const viewModeRef = useRef(viewMode);
  const firstFrameRef = useRef(true);
  const [webglError, setWebglError] = useState<string | null>(null);

  const handleFrame = useCallback((frame: CloudPreviewFrame) => {
    const resources = resourcesRef.current;
    if (!resources) {
      return;
    }

    const target = resources.positionAttribute.array as Float32Array;
    target.set(frame.positions, 0);
    resources.positionAttribute.needsUpdate = true;
    resources.geometry.setDrawRange(0, frame.pointCount);
    resources.geometry.computeBoundingBox();
    resources.geometry.computeBoundingSphere();

    if (firstFrameRef.current) {
      firstFrameRef.current = false;
      resources.fitView(viewModeRef.current);
    }
    resources.requestRender();
  }, []);

  useEffect(() => {
    const host = hostRef.current;
    if (!host) {
      return;
    }

    let animationFrame: number | null = null;
    let disposed = false;

    try {
      const renderer = new THREE.WebGLRenderer({
        antialias: true,
        alpha: true,
        powerPreference: "high-performance",
      });
      renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
      renderer.outputColorSpace = THREE.SRGBColorSpace;
      renderer.domElement.className = "cloud-canvas";
      renderer.domElement.setAttribute("aria-label", "SLAM点云三维预览");
      renderer.domElement.tabIndex = 0;
      host.appendChild(renderer.domElement);

      const scene = new THREE.Scene();
      scene.background = new THREE.Color(0xf5f8fc);
      scene.fog = new THREE.FogExp2(0xf5f8fc, 0.01);

      const perspectiveCamera = new THREE.PerspectiveCamera(48, 1, 0.01, 2_000);
      perspectiveCamera.up.set(0, 0, 1);
      perspectiveCamera.position.set(12, -12, 8);

      const topCamera = new THREE.OrthographicCamera(-10, 10, 10, -10, -500, 500);
      topCamera.up.set(0, 1, 0);
      topCamera.position.set(0, 0, 20);
      topCamera.lookAt(0, 0, 0);

      const positions = new Float32Array(PCV1_MAX_POINTS * 3);
      const positionAttribute = new THREE.BufferAttribute(positions, 3);
      positionAttribute.setUsage(THREE.DynamicDrawUsage);
      const geometry = new THREE.BufferGeometry();
      geometry.setAttribute("position", positionAttribute);
      geometry.setDrawRange(0, 0);

      const material = new THREE.PointsMaterial({
        color: 0x1769ee,
        size: 0.045,
        sizeAttenuation: true,
        transparent: true,
        opacity: 0.95,
      });
      const points = new THREE.Points(geometry, material);
      scene.add(points);

      const grid = new THREE.GridHelper(100, 100, 0x9fb8d8, 0xd8e2ef);
      grid.rotation.x = Math.PI / 2;
      grid.material.transparent = true;
      grid.material.opacity = 0.68;
      scene.add(grid);
      scene.add(new THREE.AxesHelper(1.5));

      const controls = new OrbitControls(perspectiveCamera, renderer.domElement);
      controls.enableDamping = false;
      controls.screenSpacePanning = true;
      controls.target.set(3, 1, 1);

      const activeCamera = () => (
        viewModeRef.current === "top" ? topCamera : perspectiveCamera
      );
      const render = () => {
        animationFrame = null;
        if (!disposed) {
          renderer.render(scene, activeCamera());
        }
      };
      const requestRender = () => {
        if (animationFrame === null) {
          animationFrame = window.requestAnimationFrame(render);
        }
      };

      const resize = () => {
        const width = Math.max(host.clientWidth, 1);
        const height = Math.max(host.clientHeight, 1);
        const aspect = width / height;
        renderer.setSize(width, height, false);
        perspectiveCamera.aspect = aspect;
        perspectiveCamera.updateProjectionMatrix();

        const verticalSpan = Math.max(
          topCamera.top - topCamera.bottom,
          1,
        );
        topCamera.left = -verticalSpan * aspect / 2;
        topCamera.right = verticalSpan * aspect / 2;
        topCamera.updateProjectionMatrix();
        requestRender();
      };

      const fitView = (mode: PointCloudViewMode) => {
        const box = geometry.boundingBox;
        if (!box || box.isEmpty()) {
          requestRender();
          return;
        }

        const center = box.getCenter(new THREE.Vector3());
        const size = box.getSize(new THREE.Vector3());
        const span = Math.max(size.x, size.y, size.z, 2);

        if (mode === "top") {
          const aspect = Math.max(host.clientWidth / Math.max(host.clientHeight, 1), 0.2);
          const verticalSpan = Math.max(size.y, size.x / aspect, 2) * 1.25;
          topCamera.left = -verticalSpan * aspect / 2;
          topCamera.right = verticalSpan * aspect / 2;
          topCamera.top = verticalSpan / 2;
          topCamera.bottom = -verticalSpan / 2;
          topCamera.position.set(center.x, center.y, center.z + span * 2);
          topCamera.lookAt(center);
          topCamera.updateProjectionMatrix();
          controls.object = topCamera;
          controls.target.copy(center);
          controls.update();
        } else {
          const distance = span * 1.45;
          perspectiveCamera.position.set(
            center.x + distance,
            center.y - distance,
            center.z + distance * 0.75,
          );
          controls.object = perspectiveCamera;
          controls.target.copy(center);
          controls.update();
        }
        requestRender();
      };

      controls.addEventListener("change", requestRender);
      const resizeObserver = new ResizeObserver(resize);
      resizeObserver.observe(host);

      resourcesRef.current = {
        renderer,
        scene,
        perspectiveCamera,
        topCamera,
        controls,
        geometry,
        positionAttribute,
        requestRender,
        fitView,
        resize,
      };
      resize();

      return () => {
        disposed = true;
        resizeObserver.disconnect();
        controls.removeEventListener("change", requestRender);
        controls.dispose();
        if (animationFrame !== null) {
          window.cancelAnimationFrame(animationFrame);
        }
        geometry.dispose();
        material.dispose();
        renderer.dispose();
        renderer.forceContextLoss();
        renderer.domElement.remove();
        resourcesRef.current = null;
      };
    } catch (error) {
      const detail = error instanceof Error
        ? error.message
        : "浏览器无法创建WebGL上下文";
      queueMicrotask(() => {
        if (!disposed) {
          setWebglError(detail);
        }
      });
    }
  }, []);

  useEffect(() => {
    viewModeRef.current = viewMode;
    const resources = resourcesRef.current;
    if (!resources) {
      return;
    }

    resources.controls.object = viewMode === "top"
      ? resources.topCamera
      : resources.perspectiveCamera;
    resources.fitView(viewMode);
  }, [viewMode]);

  const connection = useCloudPreviewSocket(handleFrame);
  const isStreaming = connection.streamState === "streaming" && !webglError;
  const statusText = webglError ?? connection.detail;
  const showStatusOverlay = webglError !== null
    || connection.connection === "connecting"
    || connection.connection === "reconnecting"
    || connection.streamState === "degraded"
    || connection.streamState === "ros_unavailable";

  return (
    <div className={`cloud-stage dashboard-cloud-stage cloud-stage--${viewMode}`}>
      <div ref={hostRef} className="cloud-canvas-host" />
      <div className={`cloud-live-badge cloud-live-badge--${connection.streamState}`}>
        <i />
        {isStreaming ? "实时点云" : "预览状态"}
      </div>
      {showStatusOverlay && (
        <div className="cloud-overlay-status" role="status">
          <span>⁙</span>
          <strong>{statusText}</strong>
          <small>
            {connection.connection === "reconnecting"
              ? "正在自动恢复连接"
              : "核心采集与测量任务不受预览状态影响"}
          </small>
        </div>
      )}
      <div className="cloud-stage__footer">
        <span>SLAM世界坐标系 · frame: {connection.frameId}</span>
        <span>
          {connection.pointCount.toLocaleString("zh-CN")}点
          {" · "}
          {connection.receiveFps.toFixed(1)} FPS
          {" · "}
          序号丢帧估计 {connection.droppedFrames}
        </span>
      </div>
    </div>
  );
}
