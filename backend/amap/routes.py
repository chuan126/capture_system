from __future__ import annotations

from urllib.error import HTTPError, URLError
from urllib.parse import parse_qsl, quote, urlencode
from urllib.request import Request as UrlRequest, urlopen

from fastapi import APIRouter, HTTPException, Request, Response, status
from pydantic import BaseModel, Field

from backend.device_settings import DeviceSettingsError, DeviceSettingsStore


router = APIRouter(tags=["map"])
_AMAP_REST_BASE = "https://restapi.amap.com/"
_AMAP_WEBAPI_BASE = "https://webapi.amap.com/"
_PROXY_PREFIX = "/_AMapService"
_UPSTREAM_TIMEOUT_SECONDS = 12.0


class AmapConfigUpdate(BaseModel):
    js_api_key: str = Field(min_length=1, max_length=256)
    security_js_code: str = Field(min_length=1, max_length=512)


def _store(request: Request) -> DeviceSettingsStore:
    return request.app.state.device_settings_store


def _settings(request: Request) -> tuple[str, str]:
    try:
        return _store(request).get_amap()
    except DeviceSettingsError as error:
        raise HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail=str(error)) from error


def _safe_proxy_path(proxy_path: str) -> str:
    parts = proxy_path.split("/")
    if not proxy_path or any(part in {"", ".", ".."} for part in parts) or "\\" in proxy_path:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail="地图代理路径无效")
    return quote(proxy_path, safe="/-._~")


def _upstream_url(proxy_path: str, raw_query: str, security_js_code: str) -> str:
    safe_path = _safe_proxy_path(proxy_path)
    is_style_path = proxy_path == "v4/map/styles" or proxy_path.startswith("v4/map/styles/")
    base = _AMAP_WEBAPI_BASE if is_style_path else _AMAP_REST_BASE
    parameters = [(key, value) for key, value in parse_qsl(raw_query, keep_blank_values=True) if key.lower() != "jscode"]
    parameters.append(("jscode", security_js_code))
    return f"{base}{safe_path}?{urlencode(parameters, doseq=True)}"


def _proxy_headers(upstream) -> dict[str, str]:
    headers: dict[str, str] = {}
    for name in ("Content-Type", "Cache-Control", "Expires"):
        value = upstream.headers.get(name)
        if value:
            headers[name] = value
    return headers


@router.get("/api/v1/map/config")
def get_map_config(request: Request) -> dict[str, object]:
    js_api_key, security_js_code = _settings(request)
    configured = bool(js_api_key and security_js_code)
    return {
        "configured": configured,
        "js_api_key": js_api_key if configured else None,
        "security_configured": bool(security_js_code),
        "service_host": _PROXY_PREFIX if configured else None,
    }


@router.put("/api/v1/map/config")
def put_map_config(payload: AmapConfigUpdate, request: Request) -> dict[str, object]:
    try:
        _store(request).set_amap(payload.js_api_key, payload.security_js_code)
    except DeviceSettingsError as error:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(error)) from error
    return {
        "configured": True,
        "js_api_key": payload.js_api_key.strip(),
        "security_configured": True,
        "service_host": _PROXY_PREFIX,
    }


@router.get(f"{_PROXY_PREFIX}/{{proxy_path:path}}")
def proxy_amap_service(proxy_path: str, request: Request) -> Response:
    _js_api_key, security_js_code = _settings(request)
    if not security_js_code:
        raise HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail="设备端高德地图配置未完成")
    upstream_url = _upstream_url(proxy_path, request.url.query, security_js_code)
    upstream_request = UrlRequest(
        upstream_url,
        headers={"Accept": request.headers.get("accept", "*/*"), "User-Agent": "capture-system-amap-proxy/1.0"},
        method="GET",
    )
    try:
        with urlopen(upstream_request, timeout=_UPSTREAM_TIMEOUT_SECONDS) as upstream:
            return Response(content=upstream.read(), status_code=getattr(upstream, "status", 200), headers=_proxy_headers(upstream))
    except HTTPError as error:
        return Response(content=error.read(), status_code=error.code, headers=_proxy_headers(error))
    except (URLError, TimeoutError, OSError) as error:
        raise HTTPException(status_code=status.HTTP_502_BAD_GATEWAY, detail=f"高德地图代理请求失败：{error.__class__.__name__}") from error
