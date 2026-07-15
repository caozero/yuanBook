/* ================================================================
   家庭记账 — 邀请注册页
   ================================================================ */
(function() {
    'use strict';

    var STORAGE_KEY_TOKEN = 'cp_home_ledger_token';
    var STORAGE_KEY_USERNAME = 'cp_home_ledger_username';
    var STORAGE_KEY_PERMISSIONS = 'cp_home_ledger_permissions';

    // ---- SHA1 哈希工具（优先 Web Crypto，非安全上下文自动回退到纯 JS） ----
    function SHA1_Hex(msg) {
        var cryptoApi = window.crypto || window.msCrypto;
        if (cryptoApi && cryptoApi.subtle && typeof TextEncoder !== 'undefined') {
            var data = new TextEncoder().encode(msg);
            return cryptoApi.subtle.digest('SHA-1', data).then(function(buffer) {
                var bytes = new Uint8Array(buffer);
                var hex = '';
                for (var i = 0; i < bytes.length; i++) {
                    hex += bytes[i].toString(16).padStart(2, '0');
                }
                return hex;
            });
        }

        return Promise.resolve(SHA1_Hex_Fallback(msg));
    }

    function SHA1_Hex_Fallback(msg) {
        function rotl(n, b) { return ((n << b) | (n >>> (32 - b))) >>> 0; }
        function toHex8(n) {
            return ('00000000' + (n >>> 0).toString(16)).slice(-8);
        }

        var bytes = [];
        for (var i = 0; i < msg.length; i++) {
            var code = msg.charCodeAt(i);
            if (code < 0x80) {
                bytes.push(code);
            } else if (code < 0x800) {
                bytes.push(0xc0 | (code >> 6));
                bytes.push(0x80 | (code & 0x3f));
            } else if (code < 0xd800 || code >= 0xe000) {
                bytes.push(0xe0 | (code >> 12));
                bytes.push(0x80 | ((code >> 6) & 0x3f));
                bytes.push(0x80 | (code & 0x3f));
            } else {
                i++;
                var next = i < msg.length ? msg.charCodeAt(i) : 0;
                var cp = 0x10000 + (((code & 0x3ff) << 10) | (next & 0x3ff));
                bytes.push(0xf0 | (cp >> 18));
                bytes.push(0x80 | ((cp >> 12) & 0x3f));
                bytes.push(0x80 | ((cp >> 6) & 0x3f));
                bytes.push(0x80 | (cp & 0x3f));
            }
        }

        var bitLenHi = Math.floor((bytes.length * 8) / 0x100000000);
        var bitLenLo = (bytes.length * 8) >>> 0;
        bytes.push(0x80);
        while ((bytes.length % 64) !== 56) bytes.push(0);
        bytes.push((bitLenHi >>> 24) & 0xff);
        bytes.push((bitLenHi >>> 16) & 0xff);
        bytes.push((bitLenHi >>> 8) & 0xff);
        bytes.push(bitLenHi & 0xff);
        bytes.push((bitLenLo >>> 24) & 0xff);
        bytes.push((bitLenLo >>> 16) & 0xff);
        bytes.push((bitLenLo >>> 8) & 0xff);
        bytes.push(bitLenLo & 0xff);

        var h0 = 0x67452301;
        var h1 = 0xEFCDAB89;
        var h2 = 0x98BADCFE;
        var h3 = 0x10325476;
        var h4 = 0xC3D2E1F0;
        var w = new Array(80);

        for (var offset = 0; offset < bytes.length; offset += 64) {
            for (var j = 0; j < 16; j++) {
                var idx = offset + j * 4;
                w[j] = (((bytes[idx] << 24) | (bytes[idx + 1] << 16) | (bytes[idx + 2] << 8) | bytes[idx + 3]) >>> 0);
            }
            for (var t = 16; t < 80; t++) {
                w[t] = rotl((w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16]) >>> 0, 1);
            }

            var a = h0;
            var b = h1;
            var c = h2;
            var d = h3;
            var e = h4;

            for (var k = 0; k < 80; k++) {
                var f, temp, constant;
                if (k < 20) {
                    f = (b & c) | ((~b) & d);
                    constant = 0x5A827999;
                } else if (k < 40) {
                    f = b ^ c ^ d;
                    constant = 0x6ED9EBA1;
                } else if (k < 60) {
                    f = (b & c) | (b & d) | (c & d);
                    constant = 0x8F1BBCDC;
                } else {
                    f = b ^ c ^ d;
                    constant = 0xCA62C1D6;
                }
                temp = (rotl(a, 5) + f + e + constant + w[k]) >>> 0;
                e = d;
                d = c;
                c = rotl(b, 30);
                b = a;
                a = temp;
            }

            h0 = (h0 + a) >>> 0;
            h1 = (h1 + b) >>> 0;
            h2 = (h2 + c) >>> 0;
            h3 = (h3 + d) >>> 0;
            h4 = (h4 + e) >>> 0;
        }

        return toHex8(h0) + toHex8(h1) + toHex8(h2) + toHex8(h3) + toHex8(h4);
    }

    // ---- 获取服务端 challenge ----
    function fetchChallenge(username) {
        return fetch('/api/login/challenge?username=' + encodeURIComponent(username))
            .then(function(res) {
                return res.json().then(function(data) {
                    if (!res.ok || !data.ok) throw new Error(data.message || '获取挑战码失败');
                    return data.challenge;
                });
            });
    }

    var form = document.getElementById('register-form');
    var usernameInput = document.getElementById('register-username');
    var passwordInput = document.getElementById('register-password');
    var messageBox = document.getElementById('register-message');
    var hint = document.getElementById('register-hint');
    var btnRegister = document.getElementById('btn-register');
    var currentInvite = null;
    var loggedInSession = null;

    function getInviteParams() {
        var params = new URLSearchParams(window.location.search || '');
        return {
            code: (params.get('code') || '').trim().toUpperCase()
        };
    }

    function getCode() {
        return getInviteParams().code;
    }

    function formatInviteScope(invite) {
        if (!invite || invite.autoJoinFamily === false) return '（独立用户）';
        var parts = [];
        if (Number(invite.familyId || 0) > 0) parts.push('家庭 ID：' + Number(invite.familyId));
        if (Number(invite.defaultLedgerId || 0) > 0) parts.push('默认账本 ID：' + Number(invite.defaultLedgerId));
        if (invite.familyRole) parts.push('家庭角色：' + invite.familyRole);
        return parts.length ? '（' + parts.join('，') + '）' : '（加入家庭）';
    }

    function setMessage(text, isError) {
        messageBox.textContent = text || '';
        messageBox.className = 'form-message ' + (isError === false ? 'success' : 'error') + (text ? '' : ' hidden');
    }

    function setFormEnabled(enabled) {
        usernameInput.readOnly = !enabled;
        passwordInput.readOnly = !enabled;
        btnRegister.disabled = !enabled;
    }

    function removeStoredAuthItem(storage) {
        if (!storage) return;
        storage.removeItem(STORAGE_KEY_TOKEN);
        storage.removeItem(STORAGE_KEY_USERNAME);
        storage.removeItem(STORAGE_KEY_PERMISSIONS);
    }

    function clearLocalAuth() {
        // 邀请注册页是“新用户创建入口”，进入页面时必须主动切断旧浏览器会话，避免误把邀请绑定到历史账号。
        removeStoredAuthItem(window.localStorage);
        try {
            removeStoredAuthItem(window.sessionStorage);
        } catch (err) {
            // 某些隐私模式可能禁止访问 sessionStorage；本地持久会话已清理，忽略临时存储异常。
        }
        loggedInSession = null;
        try {
            window.dispatchEvent(new CustomEvent('auth:cleared', { detail: { source: 'invite-register' } }));
        } catch (err) {
            // 兼容不支持 CustomEvent 的旧浏览器；清理动作本身不依赖事件广播。
        }
    }

    function saveAuth(data) {
        localStorage.setItem(STORAGE_KEY_TOKEN, data.token || '');
        localStorage.setItem(STORAGE_KEY_USERNAME, data.username || '');
        localStorage.setItem(STORAGE_KEY_PERMISSIONS, data.permissions ? JSON.stringify(data.permissions) : '[]');
        try {
            window.dispatchEvent(new CustomEvent('auth:changed', { detail: { source: 'invite-register', username: data.username || '' } }));
        } catch (err) {
            // 兼容旧浏览器；保存认证结果不依赖事件广播。
        }
    }

    function setRegisterMode(scopeText, expire) {
        loggedInSession = null;
        usernameInput.value = '';
        usernameInput.readOnly = false;
        passwordInput.value = '';
        passwordInput.placeholder = '请输入密码';
        passwordInput.readOnly = false;
        btnRegister.disabled = false;
        btnRegister.textContent = '创建账号并进入账本';
        hint.textContent = expire ? ('邀请链接有效' + scopeText + '，请创建新账号。有效期至：' + expire.toLocaleString()) : ('邀请链接有效' + scopeText + '，请创建新账号。');
        setMessage('', true);
        setFormEnabled(true);
        usernameInput.focus();
    }

    function verifyInvite() {
        var inviteParams = getInviteParams();
        var code = inviteParams.code;
        if (!code) {
            hint.textContent = '需要邀请注册，或者邀请链接已经过期。';
            setMessage('请使用有效的邀请注册链接访问本页面。', true);
            setFormEnabled(false);
            return;
        }

        fetch('/api/register_invite?code=' + encodeURIComponent(code))
            .then(function(res) {
                return res.json().then(function(data) {
                    if (!res.ok || !data.ok) throw new Error(data.message || '需要邀请注册，或者邀请链接已经过期。');
                    return data;
                });
            })
            .then(function(data) {
                currentInvite = data;
                var expire = data.expiresAt ? new Date(Number(data.expiresAt) * 1000) : null;
                var scopeText = formatInviteScope(data);
                // 邀请链接默认只服务“创建新账号”流程，不读取旧 token，避免浏览器历史登录态污染当前注册。
                setRegisterMode(scopeText, expire);
            })
            .catch(function(err) {
                loggedInSession = null;
                localStorage.removeItem(STORAGE_KEY_TOKEN);
                localStorage.removeItem(STORAGE_KEY_USERNAME);
                localStorage.removeItem(STORAGE_KEY_PERMISSIONS);
                setRegisterMode('', null);
                hint.textContent = '需要邀请注册，或者邀请链接已经过期。';
                setMessage(err.message || '需要邀请注册，或者邀请链接已经过期。', true);
                setFormEnabled(true);
            });
    }

    form.addEventListener('submit', function(e) {
        e.preventDefault();
        var code = getCode();
        var username = usernameInput.value.trim();
        var password = passwordInput.value;

        if (!username || !password) {
            setMessage('请输入用户名和密码。', true);
            return;
        }

        btnRegister.disabled = true;
        btnRegister.textContent = '注册中...';

        // 使用 challenge-response 保护注册密码
        fetchChallenge(username)
            .then(function(challenge) {
                return SHA1_Hex(password).then(function(passwordHash) {
                    return SHA1_Hex(challenge + passwordHash).then(function(response) {
                        return fetch('/api/register', {
                            method: 'POST',
                            headers: { 'Content-Type': 'application/json' },
                            body: JSON.stringify({
                                code: code,
                                username: username,
                                challenge: challenge,
                                response: response,
                                passwordHash: passwordHash
                            })
                        });
                    });
                });
            })
            .then(function(res) {
                return res.json().then(function(data) {
                    if (!res.ok || !data.ok) throw new Error(data.message || '注册失败');
                    return data;
                });
            })
            .then(function(data) {
                saveAuth(data);
                setMessage('注册成功，正在进入账本...', false);
                window.location.href = '/ledger.html';
            })
            .catch(function(err) {
                setMessage(err.message || '注册失败', true);
                btnRegister.disabled = false;
                btnRegister.textContent = '创建账号并进入账本';
            });
    });

    setFormEnabled(false);
    clearLocalAuth();
    verifyInvite();
})();
