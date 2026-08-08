from __future__ import annotations

from fastapi import APIRouter, HTTPException, Request, status
from pydantic import BaseModel, Field

from backend.networking.manager import NetworkManagerWifi, WifiManagerError


router = APIRouter(prefix="/api/v1/network/wifi", tags=["network"])


class WifiConnectRequest(BaseModel):
    ssid: str = Field(min_length=1, max_length=128)
    password: str | None = Field(default=None, max_length=128)


def _manager(request: Request) -> NetworkManagerWifi:
    return request.app.state.wifi_manager


@router.get("/status")
def wifi_status(request: Request) -> dict[str, object]:
    return _manager(request).status()


@router.get("/networks")
def wifi_networks(request: Request) -> dict[str, object]:
    try:
        networks = _manager(request).list_networks(rescan=False)
    except WifiManagerError as error:
        raise HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail=str(error)) from error
    return {
        "networks": [
            {
                "ssid": item.ssid,
                "signal": item.signal,
                "security": item.security,
                "secured": item.secured,
                "connected": item.connected,
            }
            for item in networks
        ]
    }


@router.post("/rescan")
def wifi_rescan(request: Request) -> dict[str, object]:
    try:
        networks = _manager(request).list_networks(rescan=True)
    except WifiManagerError as error:
        raise HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail=str(error)) from error
    return {
        "networks": [
            {
                "ssid": item.ssid,
                "signal": item.signal,
                "security": item.security,
                "secured": item.secured,
                "connected": item.connected,
            }
            for item in networks
        ]
    }


@router.post("/connect")
def wifi_connect(payload: WifiConnectRequest, request: Request) -> dict[str, object]:
    try:
        return _manager(request).connect(payload.ssid, payload.password)
    except WifiManagerError as error:
        raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail=str(error)) from error
