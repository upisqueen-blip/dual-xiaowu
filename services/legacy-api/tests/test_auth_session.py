"""会话探测接口的回归测试。

背景：/api/auth/me 在匿名时返回 401，浏览器把 4xx 记成控制台红色错误，
导致「未登录」在页面上看起来像「页面报错」。/api/auth/session 用 200 +
user=null 表达同一语义，这里锁住这个契约。
"""
from httpx import ASGITransport, AsyncClient

from api.index import app


async def test_session_anonymous_returns_200_with_null_user():
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        resp = await client.get("/api/auth/session")

    assert resp.status_code == 200, resp.text
    assert resp.json() == {"user": None}


async def test_session_with_invalid_token_still_returns_200():
    """过期/伪造的 token 也只是「未登录」，不该变成 401 或 500。"""
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        resp = await client.get(
            "/api/auth/session", headers={"Authorization": "Bearer not-a-real-token"}
        )

    assert resp.status_code == 200, resp.text
    assert resp.json() == {"user": None}


async def test_me_keeps_returning_401_for_anonymous():
    """老链接还要能用，行为不能被顺手改掉。"""
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        resp = await client.get("/api/auth/me")

    assert resp.status_code == 401
