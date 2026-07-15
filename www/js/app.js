/* ================================================================
   家庭记账 — 前端逻辑
   数据通道协议: POST /api/channel {cmd, token, data}
   ================================================================ */
(function() {
    'use strict';

    var STORAGE_KEY_TOKEN = 'cp_home_ledger_token';
    var STORAGE_KEY_USERNAME = 'cp_home_ledger_username';
    var STORAGE_KEY_PERMISSIONS = 'cp_home_ledger_permissions';
    var PAGE_SIZE = 30;
    var QUICK_CATEGORY_LIMIT = 5;
    var VOICE_ATTENTION_POLL_MS = 20000;
    var PASSIVE_REFRESH_MS = 90000;
    var VOICE_POLL_MAX_BACKOFF_MS = 120000;

    var authToken = null;
    var authUsername = null;
    var authPermissions = null;
    var loginPrecheckUsername = '';
    var loginNeedsPasswordSetup = false;
    var loginPrecheckRequestId = 0;
    var currentGroupId = 0;
    var currentGroup = null;
    var groups = [];
    var isCreatingFamilyGroup = false;
    var categories = [];
    var members = [];
    var transactions = [];
    var statsTransactions = [];
    var currentPage = 1;
    var totalTransactions = 0;
    var editingTransactionId = 0;
    var quickCategoryStats = [];
    var currentStatsPeriodType = 'month';
    var currentStatsPeriodBaseDate = new Date();
    var collapsedCategoryParents = {};
    var voicePollTimer = null;
    var passiveRefreshTimer = null;
    var voicePollInFlight = false;
    var passiveRefreshInFlight = false;
    var voicePollFailCount = 0;
    var hasPendingVoiceTransactions = false;

    // ---- 统计分页下钻与排序状态 ----
    var currentCategoryPath = [];          // [{id, name}, ...] 路径栈
    var currentParentCategoryId = 0;       // 当前父分类ID（0=根）
    var currentSortBy = 'amount';          // 'amount' | 'time'
    var currentSortOrder = 'desc';         // 'desc' | 'asc'
    var statsOffset = 0;                   // 已加载流水偏移量
    var statsHasMore = false;              // 是否还有更多流水
    var isLoadingMore = false;             // 正在加载更多
    var statsTotalTransactions = 0;        // 当前父分类下流水总数

    var billImportPollTimer = null;
    var BILL_IMPORT_POLL_MS = 2000;

    var $ = function(id) { return document.getElementById(id); };

    function dateOffset(days) {
        var d = new Date();
        d.setDate(d.getDate() + days);
        var m = String(d.getMonth() + 1).padStart(2, '0');
        var day = String(d.getDate()).padStart(2, '0');
        return d.getFullYear() + '-' + m + '-' + day;
    }

    var dom = {
        loginPanel: $('login-panel'),
        loginForm: $('login-form'),
        loginUsername: $('login-username'),
        loginPassword: $('login-password'),
        loginError: $('login-error'),
        btnLogin: $('btn-login'),
        app: $('app'),
        status: $('status'),
        authUser: $('auth-user'),
        tabLedger: $('tab-ledger'),
        tabSettings: $('tab-settings'),
        tabStats: $('tab-stats'),
        ledgerPage: $('ledger-page'),
        statsPage: $('stats-page'),
        settingsPage: $('settings-page'),
        settingsSubTabs: document.querySelectorAll('.settings-sub-tab[data-settings-page]'),
        settingsGroupsPage: $('settings-groups-page'),
        settingsCategoriesPage: $('settings-categories-page'),
        categoryFormPanel: $('category-form-panel'),
        btnToggleCategoryForm: $('btn-toggle-category-form'),
        settingsMembersPage: $('settings-members-page'),
        settingsAdminEntryLink: $('settings-admin-entry-link'),
        btnLogout: $('btn-logout'),
        currentScope: $('current-scope'),
        btnRefresh: $('btn-refresh'),
        btnToggleGroupForm: $('btn-toggle-group-form'),
        groupList: $('group-list'),
        groupForm: $('group-form'),
        groupName: $('group-name'),
        billImportForm: $('bill-import-form'),
        billImportFile: $('bill-import-file'),
        billImportResult: $('bill-import-result'),
        btnBillImport: $('btn-bill-import'),
        btnPeriodWeek: $('btn-period-week'),
        btnPeriodMonth: $('btn-period-month'),
        btnPeriodYear: $('btn-period-year'),
        btnPrevPeriod: $('btn-prev-period'),
        btnNextPeriod: $('btn-next-period'),
        currentPeriodLabel: $('current-period-label'),
        summaryExpenseCard: $('summary-expense-card'),
        summaryIncomeCard: $('summary-income-card'),
        summaryBalanceCard: $('summary-balance-card'),
        summaryExpense: $('summary-expense'),
        summaryIncome: $('summary-income'),
        summaryBalance: $('summary-balance'),
        transactionForm: $('transaction-form'),
        transactionFormTitle: $('transaction-form-title'),
        btnSaveTransaction: $('btn-save-transaction'),
        btnCancelEdit: $('btn-cancel-edit'),
        transactionDetails: $('transaction-details'),
        btnToggleTransactionDetails: $('btn-toggle-transaction-details'),
        transactionId: $('transaction-id'),
        transactionType: $('transaction-type'),
        transactionAmount: $('transaction-amount'),
        transactionCategory: $('transaction-category'),
        transactionDate: $('transaction-date'),
        transactionDescription: $('transaction-description'),
        quickCategoryList: $('quick-category-list'),
        recentPanel: document.querySelector('.recent-panel'),
        transactionList: $('transaction-list'),
        transactionTotal: $('transaction-total'),
        btnPrevPage: $('btn-prev-page'),
        btnNextPage: $('btn-next-page'),
        pageInfo: $('page-info'),
        categoryStats: $('category-stats'),
        categoryForm: $('category-form'),
        categoryId: $('category-id'),
        categoryType: $('category-type'),
        categoryParent: $('category-parent'),
        categoryName: $('category-name'),
        categorySort: $('category-sort'),
        btnSaveCategory: $('btn-save-category'),
        btnCancelCategoryEdit: $('btn-cancel-category-edit'),
        categoryList: $('category-list'),
        memberEmptyFamilyPanel: $('member-empty-family-panel'),
        memberManagePanel: $('member-manage-panel'),
        memberCreateFamilyForm: $('member-create-family-form'),
        memberFamilyName: $('member-family-name'),
        btnMemberCreateFamily: $('btn-member-create-family'),
        memberForm: $('member-form'),
        memberUsername: $('member-username'),
        memberRole: $('member-role'),
        memberList: $('member-list'),
        memberInviteIncomingPanel: $('pending-action-inbox-panel'),
        memberInviteGrid: $('member-invite-grid'),
        memberInviteSentCard: $('member-invite-sent-card'),
        memberInviteIncomingList: $('member-invite-incoming-list'),
        memberInviteSentList: $('member-invite-sent-list'),
        invitePanel: $('invite-panel'),
        inviteForm: $('invite-form'),
        inviteLink: $('invite-link'),
        inviteJoinLedger: $('invite-join-ledger'),
        inviteFamilyRole: $('invite-family-role'),
        inviteStatus: $('invite-status'),
        btnCopyInvite: $('btn-copy-invite'),
        btnCreateInvite: $('btn-create-invite'),
        pidForm: $('pid-form'),
        pidValue: $('pid-value'),
        pidLedger: $('pid-ledger'),
        pidStatus: $('pid-status'),
        pidList: $('pid-list'),
        btnCreatePid: $('btn-create-pid'),
        voiceTestSection: $('voice-test-section'),
        voiceTestForm: $('voice-test-form'),
        voiceTestText: $('voice-test-text'),
        voiceTestResult: $('voice-test-result'),
        btnVoiceTest: $('btn-voice-test'),
        toast: $('toast'),
        // ---- 统计分页新元素 ----
        btnSortAmount: $('btn-sort-amount'),
        btnSortTime: $('btn-sort-time'),
        btnStatsBack: $('btn-stats-back'),
        statsPath: $('stats-path'),
        statsLoadMore: $('stats-load-more'),
        statsEnd: $('stats-end')
    };

    function today() {
        var d = new Date();
        var m = String(d.getMonth() + 1).padStart(2, '0');
        var day = String(d.getDate()).padStart(2, '0');
        return d.getFullYear() + '-' + m + '-' + day;
    }

    function monthStart() {
        var d = new Date();
        var m = String(d.getMonth() + 1).padStart(2, '0');
        return d.getFullYear() + '-' + m + '-01';
    }

    function formatDate(d) {
        var m = String(d.getMonth() + 1).padStart(2, '0');
        var day = String(d.getDate()).padStart(2, '0');
        return d.getFullYear() + '-' + m + '-' + day;
    }

    function addDays(d, days) {
        var next = new Date(d.getFullYear(), d.getMonth(), d.getDate());
        next.setDate(next.getDate() + days);
        return next;
    }

    function addMonths(d, months) {
        return new Date(d.getFullYear(), d.getMonth() + months, 1);
    }

    function addYears(d, years) {
        return new Date(d.getFullYear() + years, 0, 1);
    }

    function getStatsPeriodRange(type, baseDate) {
        var base = baseDate || new Date();
        if (type === 'week') {
            var day = base.getDay();
            var offset = day === 0 ? -6 : 1 - day;
            var weekStart = addDays(base, offset);
            return { from: weekStart, to: addDays(weekStart, 6) };
        }
        if (type === 'year') {
            return {
                from: new Date(base.getFullYear(), 0, 1),
                to: new Date(base.getFullYear(), 11, 31)
            };
        }
        return {
            from: new Date(base.getFullYear(), base.getMonth(), 1),
            to: new Date(base.getFullYear(), base.getMonth() + 1, 0)
        };
    }

    function getStatsPeriodTitle(type, range) {
        if (!range) return '当前统计周期';
        if (type === 'week') return '周：' + formatDate(range.from) + ' 至 ' + formatDate(range.to);
        if (type === 'year') return range.from.getFullYear() + ' 年';
        return range.from.getFullYear() + ' 年 ' + String(range.from.getMonth() + 1).padStart(2, '0') + ' 月';
    }

    function updateStatsPeriodButtons() {
        var map = {
            week: dom.btnPeriodWeek,
            month: dom.btnPeriodMonth,
            year: dom.btnPeriodYear
        };
        Object.keys(map).forEach(function(type) {
            if (map[type]) map[type].classList.toggle('active', type === currentStatsPeriodType);
        });
    }

    function updateStatsPeriodLabel() {
        if (!dom.currentPeriodLabel) return;
        dom.currentPeriodLabel.textContent = getStatsPeriodTitle(currentStatsPeriodType, getStatsPeriodRange(currentStatsPeriodType, currentStatsPeriodBaseDate));
        updateStatsPeriodButtons();
    }

    function setStatsPeriod(type, baseDate, shouldRefresh) {
        currentStatsPeriodType = type || 'month';
        currentStatsPeriodBaseDate = baseDate || new Date();
        currentPage = 1;
        // 切换周期时重置下钻状态
        currentCategoryPath = [];
        currentParentCategoryId = 0;
        currentSortBy = 'amount';
        currentSortOrder = 'desc';
        statsOffset = 0;
        statsHasMore = false;
        isLoadingMore = false;
        statsTotalTransactions = 0;
        updateStatsPeriodLabel();
        if (shouldRefresh) {
            loadStats();
            loadTransactions();
        }
    }

    function shiftStatsPeriod(delta) {
        var base = currentStatsPeriodBaseDate || new Date();
        if (currentStatsPeriodType === 'week') {
            setStatsPeriod('week', addDays(base, delta * 7), true);
        } else if (currentStatsPeriodType === 'year') {
            setStatsPeriod('year', addYears(base, delta), true);
        } else {
            setStatsPeriod('month', addMonths(base, delta), true);
        }
    }

    function escapeHtml(value) {
        var div = document.createElement('div');
        div.textContent = value == null ? '' : String(value);
        return div.innerHTML;
    }

    function money(value) {
        var n = Number(value || 0);
        return '¥' + n.toFixed(2);
    }

    function showToast(message, isError) {
        dom.toast.textContent = message;
        dom.toast.className = 'toast ' + (isError ? 'error' : 'success');
        setTimeout(function() { dom.toast.classList.add('hidden'); }, 2600);
    }

    function showConfirmDialog(options) {
        options = options || {};
        return new Promise(function(resolve) {
            var overlay = document.createElement('div');
            overlay.className = 'confirm-dialog-overlay';
            overlay.setAttribute('role', 'presentation');

            var dialog = document.createElement('section');
            dialog.className = 'confirm-dialog';
            dialog.setAttribute('role', 'dialog');
            dialog.setAttribute('aria-modal', 'true');
            dialog.setAttribute('aria-labelledby', 'confirm-dialog-title');
            dialog.setAttribute('aria-describedby', 'confirm-dialog-message');

            var title = document.createElement('h2');
            title.id = 'confirm-dialog-title';
            title.textContent = options.title || '确认操作';

            var message = document.createElement('p');
            message.id = 'confirm-dialog-message';
            message.textContent = options.message || '确定要继续吗？';

            var actions = document.createElement('div');
            actions.className = 'confirm-dialog-actions';

            var cancelBtn = document.createElement('button');
            cancelBtn.type = 'button';
            cancelBtn.className = 'secondary';
            cancelBtn.textContent = options.cancelText || '取消';

            var confirmBtn = document.createElement('button');
            confirmBtn.type = 'button';
            confirmBtn.className = options.danger ? 'danger confirm-danger' : 'secondary confirm-primary';
            confirmBtn.textContent = options.confirmText || '确认';

            actions.appendChild(cancelBtn);
            actions.appendChild(confirmBtn);
            dialog.appendChild(title);
            dialog.appendChild(message);
            dialog.appendChild(actions);
            overlay.appendChild(dialog);
            document.body.appendChild(overlay);

            var done = false;
            var previousFocus = document.activeElement;

            function close(result) {
                if (done) return;
                done = true;
                document.removeEventListener('keydown', onKeyDown);
                if (overlay.parentNode) overlay.parentNode.removeChild(overlay);
                if (previousFocus && typeof previousFocus.focus === 'function') {
                    previousFocus.focus();
                }
                resolve(result);
            }

            function onKeyDown(e) {
                if (e.key === 'Escape') close(false);
            }

            overlay.addEventListener('click', function(e) {
                if (e.target === overlay) close(false);
            });
            cancelBtn.addEventListener('click', function() { close(false); });
            confirmBtn.addEventListener('click', function() { close(true); });
            document.addEventListener('keydown', onKeyDown);
            setTimeout(function() { confirmBtn.focus(); }, 0);
        });
    }

    function setStatus(text, ok) {
        dom.status.textContent = text;
        dom.status.className = 'status-pill hidden' + (ok === false ? ' bad' : '');
    }

    function showLoginError(message) {
        dom.loginError.textContent = message;
        dom.loginError.classList.remove('hidden');
    }

    function clearLoginError() {
        dom.loginError.textContent = '';
        dom.loginError.classList.add('hidden');
    }

    function saveAuth(token, username, permissions) {
        authToken = token || '';
        authUsername = username || '';
        authPermissions = permissions ? (typeof permissions === 'string' ? permissions : JSON.stringify(permissions)) : null;
        localStorage.setItem(STORAGE_KEY_TOKEN, authToken);
        localStorage.setItem(STORAGE_KEY_USERNAME, authUsername);
        if (authPermissions) {
            localStorage.setItem(STORAGE_KEY_PERMISSIONS, authPermissions);
        } else {
            localStorage.removeItem(STORAGE_KEY_PERMISSIONS);
        }
    }

    function loadAuth() {
        authToken = localStorage.getItem(STORAGE_KEY_TOKEN) || null;
        authUsername = localStorage.getItem(STORAGE_KEY_USERNAME) || null;
        authPermissions = localStorage.getItem(STORAGE_KEY_PERMISSIONS) || null;
        return !!authToken;
    }

    function clearAuth() {
        authToken = null;
        authUsername = null;
        authPermissions = null;
        localStorage.removeItem(STORAGE_KEY_TOKEN);
        localStorage.removeItem(STORAGE_KEY_USERNAME);
        localStorage.removeItem(STORAGE_KEY_PERMISSIONS);
    }

    function parsePermissionsValue(value) {
        if (!value) return [];
        if (Array.isArray(value)) return value;
        if (typeof value === 'string') {
            try {
                var parsed = JSON.parse(value);
                return Array.isArray(parsed) ? parsed : [];
            } catch (err) {
                return value.indexOf('admin') >= 0 ? ['admin'] : [];
            }
        }
        return [];
    }

    function hasAdminPermission() {
        return parsePermissionsValue(authPermissions).indexOf('admin') >= 0;
    }

    function updateAdminEntryVisibility() {
        var visible = hasAdminPermission();
        if (dom.settingsAdminEntryLink) {
            dom.settingsAdminEntryLink.classList.toggle('hidden', !visible);
        }
    }

    function showApp() {
        dom.loginPanel.classList.add('hidden');
        dom.app.classList.remove('hidden');
        dom.authUser.textContent = authUsername || '';
        dom.authUser.title = '点击刷新当前页面数据';
        dom.authUser.setAttribute('role', 'button');
        dom.authUser.setAttribute('tabindex', '0');
        dom.authUser.setAttribute('aria-label', '刷新当前页面数据');
    }

    function showLogin() {
        dom.app.classList.add('hidden');
        dom.loginPanel.classList.remove('hidden');
        clearLoginError();
    }

    function checkSession() {
        if (!authToken) return Promise.resolve(false);
        return fetch('/api/session?token=' + encodeURIComponent(authToken))
            .then(function(res) {
                if (!res.ok) {
                    console.warn('Session check failed with HTTP status:', res.status);
                    return false;
                }
                return res.text().then(function(text) {
                    try {
                        return JSON.parse(text);
                    } catch (err) {
                        console.error('Session check returned invalid JSON:', text, err);
                        showLoginError('服务端会话响应格式异常，请检查后端日志');
                        return false;
                    }
                });
            })
            .then(function(data) {
                if (!data || data === false || !data.ok || !data.username) return false;
                saveAuth(data.token || authToken, data.username, data.permissions);
                return true;
            })
            .catch(function(err) {
                console.warn('Session check request failed:', err);
                return false;
            });
    }

    // ---- SHA1 哈希工具 ----
    // 优先使用浏览器原生 Web Crypto；在 HTTP/局域网等非安全上下文中自动回退到纯 JS 实现。
    // 返回 Promise<string>，输出格式与 C++ SHA1_Hex() 一致：40 位小写十六进制字符串
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

    function resetLoginPrecheck(username) {
        loginPrecheckUsername = username || '';
        loginNeedsPasswordSetup = false;
    }

    // ---- 账号预检：仅用于判断是否需要首次设置密码 ----
    function precheckLoginUsername() {
        var username = dom.loginUsername.value.trim();
        var requestId = ++loginPrecheckRequestId;
        resetLoginPrecheck(username);
        if (!username) return Promise.resolve(false);

        return fetch('/api/login/precheck?username=' + encodeURIComponent(username))
            .then(function(res) {
                return res.json().then(function(data) {
                    if (requestId !== loginPrecheckRequestId || dom.loginUsername.value.trim() !== username) {
                        return false;
                    }
                    if (!res.ok || !data || !data.ok || !data.exists) {
                        loginNeedsPasswordSetup = false;
                        return false;
                    }
                    loginPrecheckUsername = username;
                    loginNeedsPasswordSetup = !!data.needsPasswordSetup;
                    return loginNeedsPasswordSetup;
                });
            })
            .catch(function() {
                if (requestId === loginPrecheckRequestId && dom.loginUsername.value.trim() === username) {
                    loginNeedsPasswordSetup = false;
                }
                return false;
            });
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

    // ---- 登录（Challenge-Response 模式） ----
    function login(username, password) {
        clearLoginError();
        dom.btnLogin.disabled = true;
        dom.btnLogin.textContent = '登录中...';

        // 步骤 1: 获取服务端 challenge
        fetchChallenge(username)
            .then(function(challenge) {
                // 步骤 2: 计算 response = SHA1(challenge + SHA1(password))
                return SHA1_Hex(password).then(function(passwordHash) {
                    var challengeAndHash = challenge + passwordHash;
                    return SHA1_Hex(challengeAndHash).then(function(response) {
                        console.group('[Login Debug] challenge-response');
                        console.log('username:', username);
                        console.log('password(明文):', password);
                        console.log('challenge(K):', challenge);
                        console.log('passwordHash = SHA1(password):', passwordHash);
                        console.log('challenge + passwordHash:', challengeAndHash);
                        console.log('response = SHA1(challenge + passwordHash):', response);
                        console.groupEnd();

                        // 步骤 3: 发送登录请求。普通登录不发送长期 passwordHash；
                        // 仅账号失焦预检确认空密码时，才把本次哈希作为首次设置值提交。
                        var body = {
                            username: username,
                            challenge: challenge,
                            response: response
                        };
                        if (loginPrecheckUsername === username && loginNeedsPasswordSetup) {
                            body.passwordSetup = true;
                            body.passwordHash = passwordHash;
                        }
                        return fetch('/api/login', {
                            method: 'POST',
                            headers: { 'Content-Type': 'application/json' },
                            body: JSON.stringify(body)
                        });
                    });
                });
            })
            .then(function(res) {
                return res.json().then(function(data) {
                    if (!res.ok || !data.ok) throw new Error(data.message || '登录失败');
                    return data;
                });
            })
            .then(function(data) {
                saveAuth(data.token, data.username, data.permissions);
                resetLoginPrecheck(data.username || username);
                if (data.passwordInitialized) {
                    showToast('首次登录密码已设置');
                }
                updateAdminEntryVisibility();
                showApp();
                initializeLedger();
            })
            .catch(function(err) {
                showLoginError(err.message || '登录失败');
            })
            .finally(function() {
                dom.btnLogin.disabled = false;
                dom.btnLogin.textContent = '登录';
            });
    }

    function logout() {
        stopVoiceAttentionPoll();
        clearPassiveRefreshTimer();
        if (dom.settingsAdminEntryLink) dom.settingsAdminEntryLink.classList.add('hidden');
        if (authToken) {
            fetch('/api/logout', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ token: authToken })
            }).catch(function() {});
        }
        clearAuth();
        showLogin();
    }

    function send(cmd, data) {
        setStatus('● 请求中...', true);
        return fetch('/api/channel', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ cmd: cmd, token: authToken, data: data || {} })
        })
        .then(function(res) {
            return res.json().then(function(payload) {
                var responseData = payload.data || {};
                if (!res.ok || responseData.error) {
                    var requestError = new Error(responseData.error || payload.message || payload.error || '请求失败');
                    requestError.errorCode = responseData.errorCode || payload.errorCode || '';
                    requestError.httpStatus = res.status;
                    throw requestError;
                }
                return responseData;
            });
        })
        .then(function(data) {
            setStatus('● POST通道', true);
            return data;
        })
        .catch(function(err) {
            setStatus('● 通道错误', false);
            // 只有服务端明确确认 Token 无效时才销毁本地登录态；权限或领域错误不得误踢用户下线。
            if (err.errorCode === 'AUTH_TOKEN_INVALID' || err.httpStatus === 401) {
                clearAuth();
                showLogin();
                showLoginError('登录已过期，请重新登录');
            } else {
                showToast(err.message || '请求失败', true);
            }
            throw err;
        });
    }

    function clearBillImportPoll() {
        if (billImportPollTimer) {
            clearTimeout(billImportPollTimer);
            billImportPollTimer = null;
        }
    }

    function pollBillImportStatus(taskId) {
        if (!taskId) return;

        send('ledger.bill_import.status', { taskId: taskId })
            .then(function(data) {
                var status = data.status || 'pending';

                if (status === 'pending' || status === 'processing') {
                    dom.billImportResult.textContent = '账单处理中，请稍候...';
                    billImportPollTimer = setTimeout(function() {
                        pollBillImportStatus(taskId);
                    }, BILL_IMPORT_POLL_MS);
                    return;
                }

                if (status === 'done') {
                    var summary = '导入完成：识别 ' + (data.totalRows || 0) + ' 条，成功 ' + (data.importedRows || 0) + ' 条；新增 ' + (data.insertedRows || 0) + ' 条，覆盖 ' + (data.updatedRows || 0) + ' 条，跳过 ' + (data.skippedRows || 0) + ' 条。';
                    dom.billImportResult.className = 'form-message success';
                    dom.billImportResult.textContent = summary;
                    showToast('账单导入完成');
                    setStatus('● POST通道', true);
                    clearBillImportPoll();
                    return reloadCurrentGroup();
                }

                if (status === 'failed') {
                    var errMsg = data.error || '导入失败';
                    dom.billImportResult.className = 'form-message error';
                    dom.billImportResult.textContent = errMsg;
                    showToast(errMsg, true);
                    setStatus('● 导入错误', false);
                    clearBillImportPoll();
                }
            })
            .catch(function(err) {
                // 轮询失败，继续尝试
                billImportPollTimer = setTimeout(function() {
                    pollBillImportStatus(taskId);
                }, BILL_IMPORT_POLL_MS);
            });
    }

    function uploadBillImport() {
        if (!currentGroupId) { showToast('请先选择账本', true); return; }
        if (!dom.billImportFile || !dom.billImportFile.files || !dom.billImportFile.files.length) {
            showToast('请选择要导入的账单文件', true);
            return;
        }

        var file = dom.billImportFile.files[0];
        var formData = new FormData();
        formData.append('token', authToken || '');
        formData.append('purpose', 'bill_import');
        formData.append('ledgerId', String(currentGroupId));
        formData.append('file', file);

        dom.btnBillImport.disabled = true;
        dom.btnBillImport.textContent = '上传中...';
        dom.billImportResult.className = 'form-message';
        dom.billImportResult.textContent = '正在上传...';
        setStatus('● 上传中...', true);

        return fetch('/api/upload', {
            method: 'POST',
            body: formData
        })
        .then(function(res) {
            return res.json().then(function(payload) {
                if (!res.ok || payload.error) throw new Error(payload.message || payload.error || '上传失败');
                return payload;
            });
        })
        .then(function(data) {
            dom.billImportResult.className = 'form-message success';
            dom.billImportResult.textContent = '上传完成，正在处理...';
            dom.billImportFile.value = '';
            showToast('上传完成，正在处理账单...');
            setStatus('● 处理中...', true);

            // 开始轮询任务状态
            var taskId = data.taskId;
            pollBillImportStatus(taskId);
        })
        .catch(function(err) {
            dom.billImportResult.className = 'form-message error';
            dom.billImportResult.textContent = err.message || '上传失败';
            setStatus('● 上传错误', false);
            showToast(err.message || '上传失败', true);
        })
        .finally(function() {
            dom.btnBillImport.disabled = false;
            dom.btnBillImport.textContent = '上传并导入';
        });
    }

    function refreshPidLedgerOptions() {
        if (!dom.pidLedger) return;
        dom.pidLedger.innerHTML = groups.map(function(group) {
            return '<option value="' + Number(group.id) + '">' + escapeHtml(group.familyName ? group.familyName + ' / ' + group.name : group.name) + '</option>';
        }).join('');
        if (currentGroupId) dom.pidLedger.value = String(currentGroupId);
    }

    function renderPidBindings(bindings) {
        if (!dom.pidList) return;
        dom.pidList.className = 'list-stack';
        if (!bindings.length) {
            dom.pidList.textContent = '暂无 PID，请选择账本后创建。';
            return;
        }
        dom.pidList.innerHTML = bindings.map(function(binding) {
            var options = groups.map(function(group) {
                return '<option value="' + Number(group.id) + '"' + (Number(group.id) === Number(binding.ledgerId) ? ' selected' : '') + '>' +
                    escapeHtml(group.familyName ? group.familyName + ' / ' + group.name : group.name) + '</option>';
            }).join('');
            return '<div class="ledger-list-row pid-binding-row" data-id="' + Number(binding.id) + '">' +
                '<button class="danger small pid-binding-delete" type="button">删除</button>' +
                '<div class="list-item pid-binding-identity"><strong>' + escapeHtml(binding.pid) + '</strong>' +
                (binding.accessible ? '' : '<span>已失效：无账本访问权</span>') + '</div>' +
                '<select class="pid-binding-ledger" aria-label="PID 目标账本">' + options + '</select>' +
                '<button class="secondary small pid-binding-save" type="button">保存指向</button></div>';
        }).join('');
        dom.pidList.querySelectorAll('.pid-binding-save').forEach(function(button) {
            button.addEventListener('click', function() {
                var row = this.closest('.pid-binding-row');
                var id = Number(row.getAttribute('data-id'));
                var ledgerId = Number(row.querySelector('.pid-binding-ledger').value);
                send('ledger.pid.update_ledger', { id: id, ledgerId: ledgerId }).then(function() {
                    showToast('PID 指向已更新'); loadPidBindings();
                }).catch(function(err) { showToast(err.message || 'PID 改绑失败', true); });
            });
        });
        dom.pidList.querySelectorAll('.pid-binding-delete').forEach(function(button) {
            button.addEventListener('click', function() {
                var row = this.closest('.pid-binding-row');
                var id = Number(row.getAttribute('data-id'));
                if (!confirm('确定删除该 PID？删除后外部接口将立即失效。')) return;
                send('ledger.pid.delete', { id: id }).then(function() {
                    showToast('PID 已删除'); loadPidBindings();
                }).catch(function(err) { showToast(err.message || 'PID 删除失败', true); });
            });
        });
    }

    function loadPidBindings() {
        if (!authToken || !dom.pidList) return Promise.resolve();
        refreshPidLedgerOptions();
        dom.pidStatus.textContent = '加载中...';
        return send('ledger.pid.list', {}).then(function(data) {
            var bindings = data.bindings || [];
            renderPidBindings(bindings);
            dom.pidStatus.textContent = bindings.length ? '共 ' + bindings.length + ' 个 PID 通道' : '尚未创建 PID 通道';
        }).catch(function(err) {
            dom.pidStatus.textContent = err.message || 'PID 列表加载失败';
            dom.pidStatus.classList.add('error');
        });
    }

    function createPidBinding() {
        var pid = dom.pidValue.value.trim();
        var ledgerId = Number(dom.pidLedger.value || 0);
        if (!pid || !ledgerId) { showToast('请输入 PID 并选择账本', true); return; }
        dom.btnCreatePid.disabled = true;
        return send('ledger.pid.create', { pid: pid, ledgerId: ledgerId }).then(function() {
            dom.pidValue.value = '';
            showToast('PID 已创建');
            return loadPidBindings();
        }).catch(function(err) { showToast(err.message || 'PID 创建失败', true); })
          .finally(function() { dom.btnCreatePid.disabled = false; });
    }

    function initializeLedger() {
        dom.transactionDate.value = today();
        setStatsPeriod('month', new Date(), false);
        loadGroups().finally(function() {
            schedulePassiveRefresh();
        });
        loadPidBindings();
    }

    /**
     * 统一设置账本创建表单的可见状态，并同步按钮语义状态。
     * @param {boolean} visible 是否显示创建表单。
     * @param {boolean=} shouldFocus 展开后是否聚焦账本名称输入框。
     * @returns {void}
     */
    function setGroupCreateFormVisible(visible, shouldFocus) {
        if (!dom.groupForm || !dom.btnToggleGroupForm) return;
        dom.groupForm.classList.toggle('hidden', !visible);
        dom.btnToggleGroupForm.classList.toggle('active', visible);
        dom.btnToggleGroupForm.setAttribute('aria-expanded', visible ? 'true' : 'false');
        dom.btnToggleGroupForm.title = visible ? '收起创建表单' : '创建账本';
        dom.btnToggleGroupForm.setAttribute('aria-label', visible ? '收起创建账本表单' : '创建账本');
        if (visible && shouldFocus && dom.groupName) dom.groupName.focus();
    }

    function loadGroups() {
        dom.groupList.textContent = '加载中...';
        dom.groupList.className = 'list-stack loading';
        return send('ledger.group.list')
            .then(function(data) {
                groups = data.groups || [];
                var serverCurrentGroupId = Number(data.currentGroupId || 0);
                if (serverCurrentGroupId > 0) {
                    currentGroupId = serverCurrentGroupId;
                } else if (!currentGroupId && groups.length) {
                    currentGroupId = groups[0].id;
                }
                currentGroup = groups.filter(function(g) { return Number(g.id) === Number(currentGroupId); })[0] || null;
                if (!currentGroup && groups.length) {
                    currentGroupId = Number(groups[0].id || 0);
                    currentGroup = groups[0];
                } else if (!groups.length) {
                    currentGroupId = 0;
                }
                renderGroups();
                renderMemberPageState();
                if (currentGroup) return reloadCurrentGroup();
                resetGroupDependentViews('暂无家庭组，请先在成员管理中创建家庭组');
                return loadPendingActionInbox();
            });
    }

    function resetGroupDependentViews(message) {
        dom.currentScope.textContent = message;
        dom.transactionList.textContent = message;
        dom.quickCategoryList.className = 'quick-category-list empty';
        dom.quickCategoryList.textContent = message;
        dom.categoryList.textContent = message;
        dom.memberList.textContent = message;
        renderMemberPageState();
        dom.categoryStats.textContent = '暂无数据';
        dom.summaryExpense.textContent = money(0);
        dom.summaryIncome.textContent = money(0);
        dom.summaryBalance.textContent = money(0);
        dom.summaryIncomeCard.classList.add('hidden');
        dom.summaryBalanceCard.classList.add('hidden');
        // 统计页默认隐藏收入/结余卡片（后续可通过配置开启）
        renderCategorySelects();
        // 重置统计下钻状态
        currentCategoryPath = [];
        currentParentCategoryId = 0;
        currentSortBy = 'amount';
        currentSortOrder = 'desc';
        statsOffset = 0;
        statsHasMore = false;
        isLoadingMore = false;
        statsTotalTransactions = 0;
        if (dom.statsPath) {
            dom.statsPath.innerHTML = '<span class="path-segment active" data-path="root">全部</span>';
        }
        if (dom.statsLoadMore) dom.statsLoadMore.classList.add('hidden');
        if (dom.statsEnd) dom.statsEnd.classList.add('hidden');
    }

    function reloadCurrentGroup() {
        if (!currentGroupId) return Promise.resolve();
        currentGroup = groups.filter(function(g) { return Number(g.id) === Number(currentGroupId); })[0] || currentGroup;
        dom.currentScope.textContent = currentGroup ? currentGroup.name : '当前账本';
        return Promise.all([loadCategories(), loadMembers(), loadStats(), loadTransactions(), loadQuickCategoryStats()]);
    }

    function isPageVisible() {
        return typeof document.hidden === 'undefined' || !document.hidden;
    }

    function getActivePage() {
        if (dom.statsPage.classList.contains('active')) return 'stats';
        if (dom.settingsPage.classList.contains('active')) return 'settings';
        return 'ledger';
    }

    function getActiveSettingsSubPage() {
        var active = document.querySelector('.settings-sub-tab.active[data-settings-page]');
        return active ? active.getAttribute('data-settings-page') : 'groups';
    }

    function refreshCurrentPage(isAuto) {
        if (!authToken) return Promise.resolve();
        if (!currentGroupId) {
            return loadGroups().then(function() {
                if (!isAuto) showToast('已刷新当前页面');
            });
        }

        var page = getActivePage();
        var request;
        if (page === 'stats') {
            // 统计页：静默刷新（不闪烁），不加载记账页的流水
            request = loadStats(true);
        } else if (page === 'settings') {
            var subPage = getActiveSettingsSubPage();
            if (subPage === 'categories') {
                request = Promise.all([loadCategories(), loadStats(true), loadQuickCategoryStats()]);
            } else if (subPage === 'members') {
                request = loadMembers();
            } else {
                updateVoiceTestSectionVisibility();
                request = loadGroups().then(loadPidBindings);
            }
        } else {
            request = Promise.all([loadCategories(), loadStats(), loadTransactions(), loadQuickCategoryStats()]);
        }

        return Promise.resolve(request).then(function(result) {
            if (!isAuto) {
                voicePollFailCount = 0;
                showToast('已刷新当前页面');
            }
            return result;
        });
    }

    function isVoiceParseCompleted(t) {
        return Number(t.voiceParseCompleted || 0) === 1;
    }

    function isVoiceTransactionPending(t) {
        if (Number(t.isVoiceInput || 0) !== 1) return false;
        var status = String(t.voiceParseStatus || '').toLowerCase();
        if (status === 'pending' || status === 'processing') return true;
        if (status === 'done') return !isVoiceParseCompleted(t);
        if (status === 'failed') return false;
        return false;
    }

    function getPendingVoiceTransactionIds() {
        return transactions.filter(isVoiceTransactionPending).map(function(t) {
            return Number(t.id || 0);
        }).filter(function(id) { return id > 0; });
    }

    function clearVoicePollTimer() {
        if (voicePollTimer) {
            clearTimeout(voicePollTimer);
            voicePollTimer = null;
        }
    }

    function stopVoiceAttentionPoll() {
        clearVoicePollTimer();
        hasPendingVoiceTransactions = false;
        voicePollInFlight = false;
        voicePollFailCount = 0;
    }

    function scheduleVoiceAttentionPoll(delayMs) {
        clearVoicePollTimer();
        if (!authToken || !currentGroupId || !hasPendingVoiceTransactions || !isPageVisible()) return;
        voicePollTimer = setTimeout(pollVoiceStatus, delayMs == null ? VOICE_ATTENTION_POLL_MS : delayMs);
    }

    function pollVoiceStatus() {
        if (!authToken || !currentGroupId || !hasPendingVoiceTransactions || !isPageVisible()) return;
        if (voicePollInFlight) {
            scheduleVoiceAttentionPoll(VOICE_ATTENTION_POLL_MS);
            return;
        }
        voicePollInFlight = true;
        loadTransactions()
            .then(function() {
                voicePollFailCount = 0;
                if (hasPendingVoiceTransactions) {
                    scheduleVoiceAttentionPoll(VOICE_ATTENTION_POLL_MS);
                }
            })
            .catch(function() {
                voicePollFailCount++;
                var backoff = Math.min(VOICE_POLL_MAX_BACKOFF_MS, VOICE_ATTENTION_POLL_MS * Math.max(1, voicePollFailCount));
                scheduleVoiceAttentionPoll(backoff);
            })
            .finally(function() {
                voicePollInFlight = false;
            });
    }

    function startVoiceAttentionPoll(immediate) {
        hasPendingVoiceTransactions = true;
        voicePollFailCount = 0;
        if (!isPageVisible()) return;
        if (immediate) {
            scheduleVoiceAttentionPoll(3000);
        } else {
            scheduleVoiceAttentionPoll(VOICE_ATTENTION_POLL_MS);
        }
    }

    function updateVoicePollingFromTransactions() {
        var wasPending = hasPendingVoiceTransactions;
        var pendingIds = getPendingVoiceTransactionIds();
        hasPendingVoiceTransactions = pendingIds.length > 0;
        if (hasPendingVoiceTransactions) {
            scheduleVoiceAttentionPoll(VOICE_ATTENTION_POLL_MS);
            return;
        }
        clearVoicePollTimer();
        if (wasPending && currentGroupId) {
            loadStats();
            loadQuickCategoryStats();
        }
    }

    function clearPassiveRefreshTimer() {
        if (passiveRefreshTimer) {
            clearTimeout(passiveRefreshTimer);
            passiveRefreshTimer = null;
        }
    }

    function schedulePassiveRefresh(delayMs) {
        clearPassiveRefreshTimer();
        if (!authToken || !currentGroupId || !isPageVisible()) return;
        passiveRefreshTimer = setTimeout(runPassiveRefresh, delayMs == null ? PASSIVE_REFRESH_MS : delayMs);
    }

    function runPassiveRefresh() {
        if (!authToken || !currentGroupId || !isPageVisible()) return;
        if (passiveRefreshInFlight || voicePollInFlight) {
            schedulePassiveRefresh(PASSIVE_REFRESH_MS);
            return;
        }
        passiveRefreshInFlight = true;
        refreshCurrentPage(true)
            .catch(function() {})
            .finally(function() {
                passiveRefreshInFlight = false;
                schedulePassiveRefresh(PASSIVE_REFRESH_MS);
            });
    }

    function handleVisibilityChange() {
        if (!isPageVisible()) {
            clearVoicePollTimer();
            clearBillImportPoll();
            clearPassiveRefreshTimer();
            return;
        }
        if (!authToken) return;
        refreshCurrentPage(true).catch(function() {});
        if (hasPendingVoiceTransactions) scheduleVoiceAttentionPoll(3000);
        schedulePassiveRefresh(PASSIVE_REFRESH_MS);
    }

    function renderGroups() {
        dom.groupList.className = 'list-stack';
        if (!groups.length) {
            dom.groupList.innerHTML = '<div class="empty">暂无账本</div>';
            return;
        }
        dom.groupList.innerHTML = groups.map(function(group) {
            var active = Number(group.id) === Number(currentGroupId) ? ' active' : '';
            var role = group.role || 'member';
            var canDelete = role === 'owner';
            return '<div class="ledger-list-row' + active + '">' +
                '<button class="list-item ledger-select-button' + active + '" data-id="' + group.id + '">' +
                    '<strong>' + escapeHtml(group.name) + '</strong>' +
                    '<span>' + (Number(group.id) === Number(currentGroupId) ? '当前' : escapeHtml(role)) + '</span>' +
                '</button>' +
                (canDelete ? '<button class="danger small ledger-delete-button" type="button" data-id="' + group.id + '" data-name="' + escapeHtml(group.name) + '" title="删除账本">删除</button>' : '') +
                '</div>';
        }).join('');
        dom.groupList.querySelectorAll('.list-item').forEach(function(btn) {
            btn.addEventListener('click', function() {
                var nextGroupId = Number(this.getAttribute('data-id'));
                if (!nextGroupId || nextGroupId === Number(currentGroupId)) return;
                currentGroupId = nextGroupId;
                currentPage = 1;
                renderGroups();
                send('ledger.group.set_current', { groupId: currentGroupId })
                    .then(function(data) {
                        if (Number(data.currentGroupId || 0) > 0) {
                            currentGroupId = Number(data.currentGroupId);
                        }
                        return reloadCurrentGroup();
                    })
                    .catch(function(err) {
                        showToast(err.message || '切换账本失败', true);
                        loadGroups();
                    });
            });
        });
        dom.groupList.querySelectorAll('.ledger-delete-button').forEach(function(btn) {
            btn.addEventListener('click', function(e) {
                e.preventDefault();
                e.stopPropagation();
                var groupId = Number(this.getAttribute('data-id'));
                var groupName = this.getAttribute('data-name') || '当前账本';
                deleteGroup(groupId, groupName, this);
            });
        });
    }

    function deleteGroup(groupId, groupName, button) {
        if (!groupId) return;
        var confirmMessage = '确定要删除账本“' + groupName + '”吗？\n\n此操作会删除该账本下的分类、流水、导入记录等数据，且不可恢复。';
        if (!window.confirm(confirmMessage)) return;

        if (button) {
            button.disabled = true;
            button.textContent = '删除中...';
        }
        send('ledger.group.delete', { groupId: groupId })
            .then(function() {
                showToast('账本已删除');
                if (Number(currentGroupId) === Number(groupId)) {
                    currentGroupId = 0;
                    currentGroup = null;
                    currentPage = 1;
                }
                return loadGroups();
            })
            .catch(function(err) {
                showToast(err.message || '删除账本失败', true);
                if (button) {
                    button.disabled = false;
                    button.textContent = '删除';
                }
            });
    }

    function loadCategories() {
        if (!currentGroupId) return Promise.resolve();
        return send('ledger.category.list', { ledgerId: currentGroupId })
            .then(function(data) {
                categories = data.categories || [];
                renderCategorySelects();
                renderCategoryList();
                scheduleCategoryListHeightUpdate();
            });
    }

    function getCategoryLabel(c) {
        var prefix = c.type === 'income' ? '收入 · ' : '支出 · ';
        var name = c.parentId > 0 && c.parentName ? c.parentName + ' / ' + c.name : c.name;
        return prefix + name;
    }

    function getSelectableTransactionCategories(type) {
        return categories.filter(function(c) {
            return c.type === type;
        });
    }

    function renderCategoryParentOptions() {
        var currentParent = dom.categoryParent.value || '0';
        var editingId = Number(dom.categoryId.value || 0);
        var type = dom.categoryType.value || 'expense';
        var parents = categories.filter(function(c) {
            return c.type === type && Number(c.parentId || 0) === 0 && Number(c.id) !== editingId;
        });
        dom.categoryParent.innerHTML = '<option value="0">一级分类</option>' + parents.map(function(c) {
            return '<option value="' + c.id + '">' + escapeHtml(c.name) + ' 的二级分类</option>';
        }).join('');
        dom.categoryParent.value = currentParent;
        if (dom.categoryParent.value !== currentParent) dom.categoryParent.value = '0';
    }

    function renderCategorySelects() {
        var currentCategory = dom.transactionCategory.value || '';
        var transactionType = dom.transactionType.value || 'expense';
        var filtered = getSelectableTransactionCategories(transactionType);
        dom.transactionCategory.innerHTML = filtered.length
            ? filtered.map(function(c) { return '<option value="' + c.id + '">' + escapeHtml(c.parentId > 0 && c.parentName ? c.parentName + ' / ' + c.name : c.name) + '</option>'; }).join('')
            : '<option value="">请先添加分类</option>';
        if (currentCategory) {
            dom.transactionCategory.value = currentCategory;
        }

        renderCategoryParentOptions();
        renderQuickCategories();
    }

    function loadQuickCategoryStats() {
        if (!currentGroupId) return Promise.resolve();
        return send('ledger.transaction.stats', {
            ledgerId: currentGroupId,
            dateFrom: dateOffset(-30),
            dateTo: today(),
            groupBy: 'category'
        }).then(function(stats) {
            quickCategoryStats = stats.categories || [];
            renderQuickCategories();
        });
    }

    function getQuickStat(category) {
        var categoryName = category.parentId > 0 && category.parentName ? category.parentName + ' / ' + category.name : category.name;
        return quickCategoryStats.filter(function(item) {
            return item.type === category.type && item.name === categoryName;
        })[0] || null;
    }

    function renderQuickCategories() {
        var currentCategory = Number(dom.transactionCategory.value || 0);
        if (!currentGroupId) {
            dom.quickCategoryList.className = 'quick-category-list empty';
            dom.quickCategoryList.textContent = '请选择账本';
            return;
        }

        var quickCategories = getSelectableTransactionCategories(dom.transactionType.value || 'expense')
            .map(function(c) {
                return { category: c, stat: getQuickStat(c) };
            })
            .filter(function(item) { return item.stat && Number(item.stat.count || 0) > 0; })
            .sort(function(a, b) {
                var countDiff = Number(b.stat.count || 0) - Number(a.stat.count || 0);
                if (countDiff) return countDiff;
                return Number(b.stat.amount || 0) - Number(a.stat.amount || 0);
            })
            .slice(0, QUICK_CATEGORY_LIMIT);

        if (!quickCategories.length) {
            dom.quickCategoryList.className = 'quick-category-list empty';
            dom.quickCategoryList.textContent = '近一个月暂无常用分类';
            return;
        }

        dom.quickCategoryList.className = 'quick-category-list';
        dom.quickCategoryList.innerHTML = quickCategories.map(function(item) {
            var c = item.category;
            var cls = c.type === 'income' ? 'income' : 'expense';
            var active = Number(c.id) === currentCategory ? ' active' : '';
            var label = c.parentId > 0 && c.parentName ? c.parentName + ' / ' + c.name : c.name;
            return '<button type="button" class="quick-category-button ' + cls + active + '" data-quick-category="' + c.id + '" data-quick-type="' + c.type + '">' + escapeHtml(label) + '<small>' + Number(item.stat.count || 0) + '次</small></button>';
        }).join('');
        dom.quickCategoryList.querySelectorAll('[data-quick-category]').forEach(function(btn) {
            btn.addEventListener('click', function() {
                dom.transactionType.value = this.getAttribute('data-quick-type') || 'expense';
                renderCategorySelects();
                dom.transactionCategory.value = this.getAttribute('data-quick-category') || '';
                renderQuickCategories();
                dom.transactionAmount.focus();
            });
        });
    }

    function setCategoryFormExpanded(expanded, options) {
        if (!dom.categoryFormPanel || !dom.btnToggleCategoryForm) return;
        var keepEditState = options && options.keepEditState;
        dom.categoryFormPanel.classList.toggle('hidden', !expanded);
        dom.btnToggleCategoryForm.setAttribute('aria-expanded', expanded ? 'true' : 'false');
        if (!expanded && !keepEditState) {
            dom.btnToggleCategoryForm.textContent = '添加';
        } else if (!keepEditState) {
            dom.btnToggleCategoryForm.textContent = '收起';
        }
        scheduleCategoryListHeightUpdate();
    }

    function resetCategoryForm() {
        dom.categoryId.value = '';
        dom.categoryName.value = '';
        dom.categorySort.value = '0';
        dom.categoryParent.value = '0';
        dom.btnSaveCategory.textContent = '添加';
        dom.btnCancelCategoryEdit.classList.add('hidden');
        renderCategoryParentOptions();
        setCategoryFormExpanded(false);
    }

    function startEditCategory(id) {
        var c = categories.filter(function(item) { return Number(item.id) === Number(id); })[0];
        if (!c) return;
        dom.categoryId.value = c.id;
        dom.categoryType.value = c.type || 'expense';
        renderCategoryParentOptions();
        dom.categoryParent.value = String(c.parentId || 0);
        dom.categoryName.value = c.name || '';
        dom.categorySort.value = String(c.sortOrder || 0);
        dom.btnSaveCategory.textContent = '保存';
        dom.btnCancelCategoryEdit.classList.remove('hidden');
        if (dom.btnToggleCategoryForm) dom.btnToggleCategoryForm.textContent = '编辑中';
        setCategoryFormExpanded(true, { keepEditState: true });
        dom.categoryName.focus();
    }

    function renderCategoryList() {
        if (!currentGroupId) return;
        if (!categories.length) {
            dom.categoryList.className = 'category-tree empty';
            dom.categoryList.textContent = '暂无分类';
            return;
        }
        var categoryIdMap = {};
        var childrenByParent = {};
        categories.forEach(function(c) {
            var pid = Number(c.parentId || 0);
            categoryIdMap[Number(c.id)] = c;
            if (!childrenByParent[pid]) childrenByParent[pid] = [];
            childrenByParent[pid].push(c);
        });
        Object.keys(childrenByParent).forEach(function(pid) {
            childrenByParent[pid].sort(function(a, b) {
                var sortDiff = Number(a.sortOrder || 0) - Number(b.sortOrder || 0);
                if (sortDiff) return sortDiff;
                return String(a.name || '').localeCompare(String(b.name || ''), 'zh-Hans-CN');
            });
        });
        var parents = (childrenByParent[0] || []).slice();
        var html = parents.map(function(parent) {
            var parentId = Number(parent.id);
            var cls = parent.type === 'income' ? 'income' : 'expense';
            var children = childrenByParent[parentId] || [];
            var hasChildren = children.length > 0;
            var collapsed = !!collapsedCategoryParents[parentId];
            var toggleLabel = collapsed ? '展开' : '收起';
            var childHtml = children.map(function(child) {
                var childCls = child.type === 'income' ? 'income' : 'expense';
                return '<div class="category-tree-row child ' + childCls + '">' +
                    '<span class="category-name">' + escapeHtml(child.name) + '</span>' +
                    '<small>' + (child.type === 'income' ? '收入' : '支出') + ' · 排序 ' + Number(child.sortOrder || 0) + '</small>' +
                    (Number(child.isSystem) ? '<span class="system-tag">模板</span>' : '<span class="system-tag placeholder">自定义</span>') +
                    '<button class="secondary small" data-edit-category="' + child.id + '">改</button>' +
                    (isProtectedFallbackCategory(child) ? '<button class="danger small" disabled title="兜底分类不可删除">删</button>' : '<button class="danger small" data-delete-category="' + child.id + '">删</button>') +
                    '</div>';
            }).join('');
            return '<div class="category-tree-node' + (collapsed ? ' collapsed' : '') + '">' +
                '<div class="category-tree-row parent ' + cls + '" data-toggle-category="' + parentId + '" role="button" tabindex="0" aria-expanded="' + (!collapsed) + '">' +
                '<button class="category-toggle small" type="button" data-toggle-category="' + parentId + '" aria-label="' + toggleLabel + '子分类">' + (hasChildren ? (collapsed ? '▸' : '▾') : '•') + '</button>' +
                '<strong class="category-name">' + escapeHtml(parent.name) + '</strong>' +
                '<small>' + (parent.type === 'income' ? '收入' : '支出') + ' · 子分类 ' + children.length + ' · 排序 ' + Number(parent.sortOrder || 0) + '</small>' +
                (Number(parent.isSystem) ? '<span class="system-tag">模板</span>' : '<span class="system-tag placeholder">自定义</span>') +
                '<button class="secondary small" data-edit-category="' + parent.id + '">改</button>' +
                (isProtectedFallbackCategory(parent) ? '<button class="danger small" disabled title="兜底分类不可删除">删</button>' : '<button class="danger small" data-delete-category="' + parent.id + '">删</button>') +
                '</div>' +
                (childHtml ? '<div class="category-tree-children"' + (collapsed ? ' hidden' : '') + '>' + childHtml + '</div>' : '') +
                '</div>';
        }).join('');
        var orphans = categories.filter(function(c) {
            var pid = Number(c.parentId || 0);
            return pid > 0 && !categoryIdMap[pid];
        });
        if (orphans.length) {
            html += orphans.map(function(c) {
                return '<div class="category-tree-row child ' + (c.type === 'income' ? 'income' : 'expense') + '"><span class="category-name">' + escapeHtml(c.name) + '</span><small>未找到父分类</small><span class="system-tag placeholder">异常</span><button class="secondary small" data-edit-category="' + c.id + '">改</button>' + (isProtectedFallbackCategory(c) ? '<button class="danger small" disabled title="兜底分类不可删除">删</button>' : '<button class="danger small" data-delete-category="' + c.id + '">删</button>') + '</div>';
            }).join('');
        }
        dom.categoryList.className = 'category-tree';
        dom.categoryList.innerHTML = html;
        dom.categoryList.querySelectorAll('[data-toggle-category]').forEach(function(el) {
            el.addEventListener('click', function(e) {
                if (e.target.closest('[data-edit-category], [data-delete-category]')) return;
                var id = Number(this.getAttribute('data-toggle-category'));
                if (!id || !(childrenByParent[id] || []).length) return;
                collapsedCategoryParents[id] = !collapsedCategoryParents[id];
                renderCategoryList();
            });
            el.addEventListener('keydown', function(e) {
                if (e.key !== 'Enter' && e.key !== ' ') return;
                e.preventDefault();
                var id = Number(this.getAttribute('data-toggle-category'));
                if (!id || !(childrenByParent[id] || []).length) return;
                collapsedCategoryParents[id] = !collapsedCategoryParents[id];
                renderCategoryList();
            });
        });
        dom.categoryList.querySelectorAll('[data-edit-category]').forEach(function(btn) {
            btn.addEventListener('click', function(e) {
                e.stopPropagation();
                startEditCategory(Number(this.getAttribute('data-edit-category')));
            });
        });
        dom.categoryList.querySelectorAll('[data-delete-category]').forEach(function(btn) {
            btn.addEventListener('click', function(e) {
                e.stopPropagation();
                var id = Number(this.getAttribute('data-delete-category'));
                if (!confirm('确定删除该分类？如果它有子分类，需要先手动删除子分类；已有流水会按类型转入其他支出或其他收入。')) return;
                send('ledger.category.delete', { id: id }).then(function() {
                    showToast('分类已删除，支出流水已转入其他支出，收入流水已转入其他收入');
                    resetCategoryForm();
                    loadCategories().then(function() { loadStats(); loadTransactions(); loadQuickCategoryStats(); });
                });
            });
        });
    }

    function isProtectedFallbackCategory(category) {
        if (!category) return false;
        var type = String(category.type || 'expense');
        var name = String(category.name || '');
        return (type === 'expense' && name === '其他支出') ||
            (type === 'income' && name === '其他收入');
    }

    function loadMembers() {
        if (!currentGroupId) {
            members = [];
            renderMemberPageState();
            clearSentMemberInvites('当前用户还没有家庭组');
            clearInviteRegistrationState('当前用户还没有家庭组，创建家庭组后可生成邀请链接。');
            return loadPendingActionInbox();
        }
        return send('ledger.group.members', { groupId: currentGroupId })
            .then(function(data) {
                members = data.members || [];
                renderMemberPageState();
                renderMembers();
                return Promise.all([loadPendingActionInbox(), loadSentMemberInvites(), loadCurrentInviteRegistration()]);
            });
    }

    function hasFamilyGroup() {
        return groups.length > 0 && currentGroupId > 0 && !!currentGroup;
    }

    function renderMemberPageState() {
        var hasGroup = hasFamilyGroup();
        var canManageInvites = hasGroup && canManageCurrentGroupMembers();
        if (dom.memberEmptyFamilyPanel) {
            dom.memberEmptyFamilyPanel.classList.toggle('hidden', hasGroup);
        }
        if (dom.memberManagePanel) {
            dom.memberManagePanel.classList.toggle('hidden', !hasGroup);
        }
        if (dom.memberForm) {
            dom.memberForm.classList.toggle('hidden', !canManageInvites);
        }
        if (dom.invitePanel) {
            dom.invitePanel.classList.toggle('hidden', !canManageInvites);
        }
        if (!canManageInvites) {
            setSentMemberInvitesVisible(false);
        }
        if (!hasGroup && dom.memberList) {
            dom.memberList.className = 'member-list empty';
            dom.memberList.textContent = '当前用户还没有家庭组';
        }
    }

    function createFamilyGroupFromMembersPage() {
        if (isCreatingFamilyGroup) return;
        var name = dom.memberFamilyName.value.trim();
        if (!name) return showToast('请输入家庭组名称', true);

        isCreatingFamilyGroup = true;
        dom.btnMemberCreateFamily.disabled = true;
        dom.btnMemberCreateFamily.textContent = '创建中...';

        send('ledger.group.create', { name: name })
            .then(function(data) {
                dom.memberFamilyName.value = '';
                currentGroupId = Number(data.id || 0);
                showToast('家庭组已创建');
                return loadGroups();
            })
            .catch(function(err) {
                showToast(err.message || '创建家庭组失败', true);
            })
            .finally(function() {
                isCreatingFamilyGroup = false;
                dom.btnMemberCreateFamily.disabled = false;
                dom.btnMemberCreateFamily.textContent = '创建家庭组';
            });
    }

    function renderMembers() {
        if (!members.length) {
            dom.memberList.className = 'member-list empty';
            dom.memberList.textContent = '暂无成员';
            return;
        }
        dom.memberList.className = 'member-list';
        dom.memberList.innerHTML = members.map(function(m) {
            var canRemove = m.username !== authUsername;
            return '<div class="member-row">' +
                '<div><strong>' + escapeHtml(m.username) + '</strong><span>' + escapeHtml(m.role || 'member') + '</span></div>' +
                (canRemove ? '<button class="secondary small" data-remove-member="' + escapeHtml(m.username) + '">移除</button>' : '<span class="muted">当前用户</span>') +
                '</div>';
        }).join('');
        dom.memberList.querySelectorAll('[data-remove-member]').forEach(function(btn) {
            btn.addEventListener('click', function() {
                var username = this.getAttribute('data-remove-member');
                if (!confirm('确定将 ' + username + ' 移出当前账本？')) return;
                send('ledger.group.remove_member', { groupId: currentGroupId, username: username }).then(function() {
                    showToast('成员已移除');
                    loadMembers();
                });
            });
        });
    }

    function canManageCurrentGroupMembers() {
        return currentGroup && (currentGroup.role === 'owner' || currentGroup.role === 'admin');
    }

    function setIncomingMemberInvitesVisible(visible) {
        if (dom.memberInviteIncomingPanel) {
            dom.memberInviteIncomingPanel.classList.toggle('hidden', !visible);
        }
    }

    function setSentMemberInvitesVisible(visible) {
        if (dom.memberInviteGrid) {
            dom.memberInviteGrid.classList.toggle('hidden', !visible);
        }
        if (dom.memberInviteSentCard) {
            dom.memberInviteSentCard.classList.toggle('hidden', !visible);
        }
    }

    function formatPendingActionTime(timestamp) {
        var ts = Number(timestamp || 0);
        if (ts <= 0) return '长期有效';
        var date = new Date(ts * 1000);
        if (isNaN(date.getTime())) return '时间未知';
        return date.toLocaleString();
    }

    function clearSentMemberInvites(message) {
        setSentMemberInvitesVisible(false);
        if (dom.memberInviteSentList) {
            dom.memberInviteSentList.className = 'member-invite-list empty';
            dom.memberInviteSentList.textContent = message || '暂无已发送邀请';
        }
    }

    function renderIncomingMemberInvites(invites) {
        if (!dom.memberInviteIncomingList) return;
        if (!invites || !invites.length) {
            setIncomingMemberInvitesVisible(false);
            dom.memberInviteIncomingList.className = 'member-invite-list empty';
            dom.memberInviteIncomingList.textContent = '';
            return;
        }
        setIncomingMemberInvitesVisible(true);
        dom.memberInviteIncomingList.className = 'member-invite-list';
        dom.memberInviteIncomingList.innerHTML = invites.map(function(invite) {
            return '<div class="member-invite-row">' +
                '<div><strong>' + escapeHtml(invite.groupName || ('账本 #' + invite.groupId)) + '</strong>' +
                '<span>邀请人：' + escapeHtml(invite.invitedBy || '') + ' · 角色：' + escapeHtml(invite.role || 'member') + ' · 有效期至：' + escapeHtml(formatPendingActionTime(invite.expiresAt)) + '</span></div>' +
                '<div class="member-invite-actions">' +
                '<button class="small" data-accept-member-invite="' + escapeHtml(invite.id) + '">接受</button>' +
                '<button class="secondary small" data-reject-member-invite="' + escapeHtml(invite.id) + '">拒绝</button>' +
                '</div>' +
                '</div>';
        }).join('');
        dom.memberInviteIncomingList.querySelectorAll('[data-accept-member-invite]').forEach(function(btn) {
            btn.addEventListener('click', function() {
                var inviteId = Number(this.getAttribute('data-accept-member-invite') || 0);
                send('ledger.group.invite.accept', { inviteId: inviteId }).then(function() {
                    showToast('已接受邀请并加入账本');
                    loadGroups();
                });
            });
        });
        dom.memberInviteIncomingList.querySelectorAll('[data-reject-member-invite]').forEach(function(btn) {
            btn.addEventListener('click', function() {
                var inviteId = Number(this.getAttribute('data-reject-member-invite') || 0);
                send('ledger.group.invite.reject', { inviteId: inviteId }).then(function() {
                    showToast('已拒绝邀请');
                    loadPendingActionInbox();
                });
            });
        });
    }

    function renderSentMemberInvites(invites) {
        if (!dom.memberInviteSentList) return;
        if (!canManageCurrentGroupMembers()) {
            setSentMemberInvitesVisible(false);
            dom.memberInviteSentList.className = 'member-invite-list empty';
            dom.memberInviteSentList.textContent = '';
            return;
        }
        if (!invites || !invites.length) {
            setSentMemberInvitesVisible(false);
            dom.memberInviteSentList.className = 'member-invite-list empty';
            dom.memberInviteSentList.textContent = '';
            return;
        }
        setSentMemberInvitesVisible(true);
        dom.memberInviteSentList.className = 'member-invite-list';
        dom.memberInviteSentList.innerHTML = invites.map(function(invite) {
            return '<div class="member-invite-row">' +
                '<div><strong>' + escapeHtml(invite.targetUsername || '') + '</strong>' +
                '<span>角色：' + escapeHtml(invite.role || 'member') + ' · 邀请人：' + escapeHtml(invite.invitedBy || '') + ' · 有效期至：' + escapeHtml(formatPendingActionTime(invite.expiresAt)) + '</span></div>' +
                '<button class="secondary small" data-cancel-member-invite="' + escapeHtml(invite.id) + '">取消邀请</button>' +
                '</div>';
        }).join('');
        dom.memberInviteSentList.querySelectorAll('[data-cancel-member-invite]').forEach(function(btn) {
            btn.addEventListener('click', function() {
                var inviteId = Number(this.getAttribute('data-cancel-member-invite') || 0);
                send('ledger.group.invite.cancel', { inviteId: inviteId }).then(function() {
                    showToast('邀请已取消');
                    loadMembers();
                });
            });
        });
    }

    function loadPendingActionInbox() {
        if (dom.memberInviteIncomingList) {
            setIncomingMemberInvitesVisible(false);
            dom.memberInviteIncomingList.className = 'member-invite-list loading';
            dom.memberInviteIncomingList.textContent = '正在加载待处理邀请...';
        }
        return send('ledger.group.invites.incoming', {})
            .then(function(data) { renderIncomingMemberInvites(data.invites || []); })
            .catch(function(err) {
                if (dom.memberInviteIncomingList) {
                    setIncomingMemberInvitesVisible(true);
                    dom.memberInviteIncomingList.className = 'member-invite-list empty';
                    dom.memberInviteIncomingList.textContent = err.message || '待处理邀请加载失败';
                }
            });
    }

    function loadSentMemberInvites() {
        if (!currentGroupId) {
            clearSentMemberInvites('当前用户还没有家庭组');
            return Promise.resolve();
        }
        if (dom.memberInviteSentList) {
            setSentMemberInvitesVisible(canManageCurrentGroupMembers());
            dom.memberInviteSentList.className = 'member-invite-list loading';
            dom.memberInviteSentList.textContent = canManageCurrentGroupMembers() ? '正在加载已发送邀请...' : '';
        }
        if (!canManageCurrentGroupMembers()) {
            renderSentMemberInvites([]);
            return Promise.resolve();
        }
        return send('ledger.group.invites.sent', { groupId: currentGroupId })
            .then(function(data) { renderSentMemberInvites(data.invites || []); })
            .catch(function(err) {
                if (dom.memberInviteSentList) {
                    setSentMemberInvitesVisible(true);
                    dom.memberInviteSentList.className = 'member-invite-list empty';
                    dom.memberInviteSentList.textContent = err.message || '已发送邀请加载失败';
                }
            });
    }

    function buildInviteLink(code) {
        var params = new URLSearchParams();
        params.set('code', code || '');
        return window.location.origin + '/register.html?' + params.toString();
    }

    function getInviteRequestData() {
        var useDefaultLedger = !dom.inviteJoinLedger || dom.inviteJoinLedger.checked;
        return {
            inviteEntry: 'ledger_directory',
            inviteType: 'join_family',
            // sourceLedgerId 始终声明当前账本上下文；defaultLedgerId 仅控制注册后的默认账本偏好。
            sourceLedgerId: currentGroupId,
            familyId: 0,
            defaultLedgerId: useDefaultLedger ? currentGroupId : 0,
            familyRole: dom.inviteFamilyRole ? dom.inviteFamilyRole.value : 'member'
        };
    }

    function formatInviteExpiresAt(expiresAt) {
        var ts = Number(expiresAt || 0);
        if (ts <= 0) return '';
        var date = new Date(ts * 1000);
        if (isNaN(date.getTime())) return '';
        return date.toLocaleString();
    }

    function clearInviteRegistrationState(message) {
        if (dom.inviteLink) dom.inviteLink.value = '';
        if (dom.inviteStatus) {
            dom.inviteStatus.textContent = message || '当前选项下没有可复用的有效邀请链接，可点击“邀请注册”生成新链接。';
        }
    }

    function setInviteLinkFromInvite(invite, message) {
        if (!invite || !invite.code) {
            clearInviteRegistrationState(message);
            return;
        }
        var link = buildInviteLink(invite.code);
        if (dom.inviteLink) dom.inviteLink.value = link;
        if (dom.inviteStatus) {
            var expireText = formatInviteExpiresAt(invite.expiresAt);
            dom.inviteStatus.textContent = message || ('已找到当前创建者的有效邀请链接，可直接复制使用' + (expireText ? '，有效期至：' + expireText : '') + '。');
        }
    }

    function loadCurrentInviteRegistration() {
        if (!dom.inviteLink) return Promise.resolve();

        var data = getInviteRequestData();
        if (!currentGroupId) {
            clearInviteRegistrationState('请先选择账本，再查询或创建加入家庭的注册邀请。');
            return Promise.resolve();
        }
        if (dom.inviteStatus) dom.inviteStatus.textContent = '正在查找当前选项下的有效邀请链接...';
        return send('ledger.invite.current', data)
            .then(function(result) {
                if (result && result.hasInvite) {
                    setInviteLinkFromInvite(result);
                } else {
                    clearInviteRegistrationState();
                }
            })
            .catch(function(err) {
                clearInviteRegistrationState(err.message || '查询有效邀请链接失败，可重新生成。');
            });
    }

    function copyInviteLink() {
        if (!dom.inviteLink || !dom.inviteLink.value) {
            showToast('请先生成邀请链接', true);
            return;
        }
        dom.inviteLink.focus();
        dom.inviteLink.select();
        if (navigator.clipboard && navigator.clipboard.writeText) {
            navigator.clipboard.writeText(dom.inviteLink.value)
                .then(function() { showToast('邀请链接已复制'); })
                .catch(function() { showToast('已选中链接，请手动复制'); });
        } else {
            showToast('已选中链接，请手动复制');
        }
    }

    function createInviteRegistration() {
        var data = getInviteRequestData();
        if (!currentGroupId) { showToast('请先选择账本', true); return; }
        var link = '';
        dom.btnCreateInvite.disabled = true;
        if (dom.btnCopyInvite) dom.btnCopyInvite.disabled = true;
        dom.btnCreateInvite.textContent = '生成中...';
        dom.inviteStatus.textContent = '正在保存邀请链接...';
        return send('ledger.invite.create', data)
            .then(function(result) {
                if (!result || result.inviteType !== 'join_family' || Number(result.familyId || 0) <= 0) {
                    throw new Error('服务端返回的邀请作用域不是当前家庭，已拒绝展示该链接');
                }
                link = buildInviteLink(result.code || '');
                dom.inviteLink.value = link;
                dom.inviteStatus.textContent = '家庭邀请链接已生成，注册后会加入当前账本所属家庭。';
                copyInviteLink();
                showToast('邀请注册链接已生成');
            })
            .catch(function(err) {
                dom.inviteStatus.textContent = err.message || '邀请链接生成失败';
            })
            .finally(function() {
                dom.btnCreateInvite.disabled = false;
                if (dom.btnCopyInvite) dom.btnCopyInvite.disabled = false;
                dom.btnCreateInvite.textContent = '生成家庭邀请';
            });
    }

    function renderVoiceTestResult(result) {
        result = result || {};
        var html = '<div class="voice-test-parsed">';
        html += '<div class="panel-title"><h3>语音文本账目已录入</h3></div>';
        html += '<div class="form-message success">流水 #' + escapeHtml(result.transactionId || '') + ' 已创建，金额暂为 0，后台会从解析队列自动解析并回填。</div>';
        html += '<div class="empty">如果解析失败或金额仍为 0，可在流水列表中点击“解析”重新加入解析队列。</div>';
        html += '</div>';
        dom.voiceTestResult.innerHTML = html;
        dom.voiceTestResult.classList.remove('hidden');
    }

    function updateVoiceTestSectionVisibility() {
        if (hasAdminPermission()) {
            dom.voiceTestSection.classList.remove('hidden');
        } else {
            dom.voiceTestSection.classList.add('hidden');
        }
    }

    function getFilterData() {
        var range = getStatsPeriodRange(currentStatsPeriodType, currentStatsPeriodBaseDate);
        return {
            ledgerId: currentGroupId,
            dateFrom: formatDate(range.from),
            dateTo: formatDate(range.to),
            categoryId: 0,
            type: '',
            parentCategoryId: currentParentCategoryId
        };
    }

    function updateScrollableListHeight(options) {
        if (!options || !options.list || !options.panel || !options.page) return;
        var list = options.list;
        var panel = options.panel;
        var page = options.page;

        if (!page.classList.contains('active')) {
            list.style.height = '';
            list.style.maxHeight = '';
            return;
        }

        // 测量前先撤销上一次的固定高度，避免旧高度反向污染当前内容高度。
        list.style.height = '';
        list.style.maxHeight = '';

        var viewportHeight = window.innerHeight || document.documentElement.clientHeight || 0;
        if (!viewportHeight) return;

        var panelRect = panel.getBoundingClientRect();
        var listRect = list.getBoundingClientRect();
        var nextElement = list.nextElementSibling;
        var panelStyle = window.getComputedStyle(panel);
        var nextElementStyle = nextElement ? window.getComputedStyle(nextElement) : null;
        var panelPaddingBottom = parseFloat(panelStyle.paddingBottom || '0') || 0;
        var listMarginBottom = parseFloat(window.getComputedStyle(list).marginBottom || '0') || 0;
        var nextElementHeight = nextElement ? nextElement.offsetHeight : 0;
        var nextElementMarginTop = nextElementStyle ? (parseFloat(nextElementStyle.marginTop || '0') || 0) : 0;
        var viewportBottomSpacing = viewportHeight <= 720 ? 12 : 20;
        var minHeight = typeof options.minHeight === 'number'
            ? options.minHeight
            : (viewportHeight <= 720 ? 220 : 300);
        var availableHeight = Math.floor(viewportHeight - listRect.top - nextElementHeight - nextElementMarginTop - panelPaddingBottom - listMarginBottom - viewportBottomSpacing);
        var panelVisibleHeight = Math.floor(viewportHeight - panelRect.top - panelPaddingBottom - viewportBottomSpacing);

        if (panelVisibleHeight > 0) {
            availableHeight = Math.min(availableHeight, panelVisibleHeight - (listRect.top - panelRect.top));
        }

        if (availableHeight > minHeight) {
            list.style.maxHeight = availableHeight + 'px';
            // 默认让少量内容按自然高度收缩；仅明确要求时才填满剩余视口。
            if (options.fillAvailableHeight === true) {
                list.style.height = availableHeight + 'px';
            }
        }
    }

    function updateRecentTransactionListHeight() {
        updateScrollableListHeight({
            page: dom.ledgerPage,
            panel: dom.recentPanel,
            list: dom.transactionList,
            minHeight: (window.innerHeight || document.documentElement.clientHeight || 0) <= 720 ? 160 : 220,
            fillAvailableHeight: false
        });
    }

    function updateCategoryListHeight() {
        if (!dom.settingsPage || !dom.settingsCategoriesPage || !dom.categoryList) return;
        updateScrollableListHeight({
            page: dom.settingsPage,
            panel: dom.settingsCategoriesPage.querySelector('.panel'),
            list: dom.categoryList,
            minHeight: (window.innerHeight || document.documentElement.clientHeight || 0) <= 720 ? 220 : 280,
            fillAvailableHeight: true
        });
    }

    function scheduleCategoryListHeightUpdate() {
        if (window.requestAnimationFrame) {
            window.requestAnimationFrame(updateCategoryListHeight);
            return;
        }
        setTimeout(updateCategoryListHeight, 0);
    }

    function scheduleRecentTransactionListHeightUpdate() {
        if (window.requestAnimationFrame) {
            window.requestAnimationFrame(updateRecentTransactionListHeight);
            return;
        }
        setTimeout(updateRecentTransactionListHeight, 0);
    }

    function scheduleAllAdaptiveListHeightUpdates() {
        scheduleRecentTransactionListHeightUpdate();
        scheduleCategoryListHeightUpdate();
    }

    function loadTransactions() {
        if (!currentGroupId) return Promise.resolve();
        var data = getFilterData();
        data.offset = (currentPage - 1) * PAGE_SIZE;
        data.limit = PAGE_SIZE;
        dom.transactionList.className = 'transaction-list loading';
        dom.transactionList.textContent = '加载中...';
        return send('ledger.transaction.list', data)
            .then(function(result) {
                transactions = result.transactions || [];
                totalTransactions = Number(result.total || 0);
                renderTransactions();
                updateVoicePollingFromTransactions();
                scheduleRecentTransactionListHeightUpdate();
            });
    }

    function buildTransactionActionBar(t) {
        var id = Number(t && t.id || 0);
        return '<div class="transaction-detail-actions">' +
            '<button type="button" class="danger small" data-transaction-delete="' + id + '">删除</button>' +
            '<button type="button" class="secondary small" data-transaction-edit="' + id + '">编辑</button>' +
            '</div>';
    }

    function buildRecentTransactionDetailRows(t, sign, amount) {
        var rows = [];

        // 详情栏只承载语音流水独有的补充信息与操作按钮；普通流水展开后仅显示“删除 / 编辑”。
        if (Number(t.isVoiceInput || 0) === 1) {
            rows.push('<div class="detail-row"><span>解析完成</span><span>' + (isVoiceParseCompleted(t) ? '是' : '否') + '</span></div>');
            if (t.voiceText) {
                rows.push('<div class="detail-row description-row"><span>语音文本</span><span>' + escapeHtml(t.voiceText) + '</span></div>');
            }
        }

        rows.push(buildTransactionActionBar(t));
        return rows.join('');
    }

    function findTransactionById(id) {
        id = Number(id || 0);
        for (var i = 0; i < transactions.length; i++) {
            if (Number(transactions[i].id || 0) === id) return transactions[i];
        }
        for (var j = 0; j < statsTransactions.length; j++) {
            if (Number(statsTransactions[j].id || 0) === id) return statsTransactions[j];
        }
        return null;
    }

    function refreshAfterTransactionDeleted() {
        var maxPageAfterDelete = Math.max(1, Math.ceil(Math.max(0, totalTransactions - 1) / PAGE_SIZE));
        if (currentPage > maxPageAfterDelete) currentPage = maxPageAfterDelete;
        return Promise.all([loadStats(), loadTransactions(), loadQuickCategoryStats()]);
    }

    function deleteTransaction(id) {
        id = Number(id || 0);
        if (!id) return;
        var t = findTransactionById(id);
        var summary = t ? ((t.type === 'income' ? '收入 ' : '支出 ') + money(t.amount) + ' · ' + (t.parentCategoryName ? t.parentCategoryName + ' / ' + t.categoryName : t.categoryName || '未分类')) : '这条流水';
        return showConfirmDialog({
            title: '删除流水',
            message: '确定要删除“' + summary + '”吗？删除后无法恢复。',
            confirmText: '删除',
            cancelText: '取消',
            danger: true
        }).then(function(confirmed) {
            if (!confirmed) return false;
            return send('ledger.transaction.delete', { id: id })
                .then(function() {
                    showToast('流水已删除');
                    return refreshAfterTransactionDeleted();
                });
        });
    }

    function bindTransactionActionEvents(rootElement) {
        var root = rootElement || dom.transactionList;
        if (!root) return;

        Array.prototype.forEach.call(root.querySelectorAll('[data-transaction-edit]'), function(btn) {
            if (btn._transactionEditBound) return;
            btn._transactionEditBound = true;
            btn.addEventListener('click', function(e) {
                e.preventDefault();
                e.stopPropagation();
                var id = Number(this.getAttribute('data-transaction-edit') || 0);
                if (!id) return;
                startEditTransaction(id);
            });
        });

        Array.prototype.forEach.call(root.querySelectorAll('[data-transaction-delete]'), function(btn) {
            if (btn._transactionDeleteBound) return;
            btn._transactionDeleteBound = true;
            btn.addEventListener('click', function(e) {
                e.preventDefault();
                e.stopPropagation();
                var id = Number(this.getAttribute('data-transaction-delete') || 0);
                if (!id) return;
                this.disabled = true;
                deleteTransaction(id).finally(function() {
                    if (btn && btn.isConnected) btn.disabled = false;
                });
            });
        });
    }

    function bindRecentTransactionExpandEvents() {
        if (!dom.transactionList) return;
        Array.prototype.forEach.call(dom.transactionList.querySelectorAll('[data-recent-transaction-id]'), function(el) {
            if (el._recentExpandBound) return;
            el._recentExpandBound = true;
            el.addEventListener('click', function(e) {
                if (e.target.closest('button')) return;
                var panel = this.querySelector('.transaction-detail-panel');
                if (!panel) return;
                var shouldExpand = panel.classList.contains('hidden');
                Array.prototype.forEach.call(dom.transactionList.querySelectorAll('.transaction-item.expanded'), function(other) {
                    if (other !== el) {
                        other.classList.remove('expanded');
                        var otherPanel = other.querySelector('.transaction-detail-panel');
                        if (otherPanel) otherPanel.classList.add('hidden');
                    }
                });
                panel.classList.toggle('hidden', !shouldExpand);
                this.classList.toggle('expanded', shouldExpand);
                // 展开详情会改变内容总高度，需要重新计算滚动上限。
                scheduleRecentTransactionListHeightUpdate();
            });
        });
    }

    function renderTransactions() {
        var maxPage = Math.max(1, Math.ceil(totalTransactions / PAGE_SIZE));
        if (currentPage > maxPage) {
            currentPage = maxPage;
            loadTransactions();
            return;
        }

        dom.transactionTotal.textContent = '共 ' + totalTransactions + ' 条';
        dom.pageInfo.textContent = '共 ' + totalTransactions + ' 条 · 第 ' + currentPage + ' / ' + maxPage + ' 页';
        dom.btnPrevPage.disabled = !currentGroupId || currentPage <= 1;
        dom.btnNextPage.disabled = !currentGroupId || currentPage >= maxPage;

        if (!transactions.length) {
            dom.transactionList.className = 'transaction-list empty';
            dom.transactionList.textContent = currentGroupId ? '暂无流水' : '请选择账本';
            return;
        }
        dom.transactionList.className = 'transaction-list';
        dom.transactionList.innerHTML = transactions.map(function(t) {
            var cls = t.type === 'income' ? 'income' : 'expense';
            if (editingTransactionId && Number(t.id || 0) === Number(editingTransactionId)) cls += ' editing';
            var sign = t.type === 'income' ? '+' : '-';
            var isVoice = Number(t.isVoiceInput || 0) === 1;
            var amount = Number(t.amount || 0);
            var voiceStatus = isVoice ? (' · 语音录入' + (t.voiceParseStatus ? ' · ' + t.voiceParseStatus : '')) : '';
            var voiceError = isVoice && t.voiceParseError ? '<small class="form-message error">解析失败：' + escapeHtml(t.voiceParseError) + '</small>' : '';
            var voiceCanReparse = isVoice && String(t.voiceParseStatus || '').toLowerCase() !== 'pending' && String(t.voiceParseStatus || '').toLowerCase() !== 'processing';
            var parseButton = voiceCanReparse && !isVoiceParseCompleted(t) ? '<button type="button" class="secondary" data-voice-parse="' + t.id + '">解析</button>' : '';
            var detailRows = buildRecentTransactionDetailRows(t, sign, amount);
            return '<article class="transaction-item ' + cls + ' transaction-item-expandable" data-recent-transaction-id="' + t.id + '">' +
                '<div class="transaction-main">' +
                '<strong>' + escapeHtml(t.parentCategoryName ? t.parentCategoryName + ' / ' + t.categoryName : t.categoryName) + '</strong>' +
                '<span>' + escapeHtml(t.description || '无备注') + '</span>' +
                '<small>' + escapeHtml(t.date || '') + ' · ' + escapeHtml(t.createdBy || '') + escapeHtml(voiceStatus) + '</small>' +
                voiceError +
                '</div>' +
                '<div class="transaction-amount">' + sign + money(t.amount) + parseButton + '</div>' +
                '<div class="transaction-detail-panel hidden">' + detailRows + '</div>' +
                '</article>';
        }).join('');

        bindRecentTransactionExpandEvents();
        bindTransactionActionEvents();

        bindVoiceParseEvents(dom.transactionList);
    }

    function bindVoiceParseEvents(rootElement) {
        var root = rootElement || dom.transactionList;
        if (!root) return;

        Array.prototype.forEach.call(root.querySelectorAll('[data-voice-parse]'), function(btn) {
            if (btn._voiceParseBound) return;
            btn._voiceParseBound = true;
            btn.addEventListener('click', function(e) {
                e.preventDefault();
                e.stopPropagation();
                var id = Number(this.getAttribute('data-voice-parse') || 0);
                if (!id) return;
                this.disabled = true;
                this.textContent = '已加入...';
                send('ledger.voice.parse', { id: id })
                    .then(function() {
                        showToast('已加入解析队列');
                        startVoiceAttentionPoll(true);
                        loadTransactions();
                        loadStats(true);
                    })
                    .catch(function(err) {
                        showToast(err.message || '加入解析队列失败', true);
                        loadTransactions();
                        loadStats(true);
                    });
            });
        });
    }

    // ===== 统计下钻 / 路径 / 排序 / 增量加载 =====

    function renderPathBreadcrumb() {
        if (!dom.statsPath) return;
        var html = '';
        // 根路径（全部）
        html += '<span class="path-segment' + (currentCategoryPath.length === 0 ? ' active' : '') + '" data-path="root">全部</span>';
        // 路径分段
        for (var i = 0; i < currentCategoryPath.length; i++) {
            var seg = currentCategoryPath[i];
            html += '<span class="path-separator">/</span>';
            var isLast = (i === currentCategoryPath.length - 1);
            html += '<span class="path-segment' + (isLast ? ' active' : '') + '" data-path="' + i + '">' + escapeHtml(seg.name) + '</span>';
        }
        dom.statsPath.innerHTML = html;

        // 点击非激活路径段回到该层级
        dom.statsPath.querySelectorAll('.path-segment[data-path]').forEach(function(el) {
            if (el.classList.contains('active')) return;
            el.addEventListener('click', function() {
                var p = this.getAttribute('data-path');
                if (p === 'root') {
                    // 回到根
                    while (currentCategoryPath.length > 0) currentCategoryPath.pop();
                    currentParentCategoryId = 0;
                } else {
                    var idx = parseInt(p, 10);
                    if (!isNaN(idx) && idx >= 0 && idx < currentCategoryPath.length) {
                        currentCategoryPath = currentCategoryPath.slice(0, idx + 1);
                        currentParentCategoryId = currentCategoryPath[idx].id;
                    }
                }
                statsOffset = 0;
                statsHasMore = false;
                isLoadingMore = false;
                statsTotalTransactions = 0;
                loadStats();
            });
        });
    }

    function loadStats(silent) {
        if (!currentGroupId) return Promise.resolve();
        var filter = getFilterData();

        // 更新返回按钮可见性
        if (dom.btnStatsBack) {
            dom.btnStatsBack.classList.toggle('hidden', currentCategoryPath.length === 0);
        }

        // 更新排序按钮激活状态
        if (dom.btnSortAmount) {
            dom.btnSortAmount.classList.toggle('active', currentSortBy === 'amount');
        }
        if (dom.btnSortTime) {
            dom.btnSortTime.classList.toggle('active', currentSortBy === 'time');
        }

        // 1. 获取分类统计
        var statsPromise = send('ledger.transaction.stats', {
            ledgerId: filter.ledgerId,
            dateFrom: filter.dateFrom,
            dateTo: filter.dateTo,
            groupBy: 'category',
            parentCategoryId: currentParentCategoryId
        });

        // 非静默模式才显示加载中（避免自动刷新闪烁）
        if (!silent) {
            dom.categoryStats.className = 'stats-list loading';
            dom.categoryStats.textContent = '加载中...';
        }

        // 根节点只加载分类统计，不加载流水
        var isRootLevel = currentCategoryPath.length === 0;
        if (isRootLevel) {
            return statsPromise.then(function(stats) {
                renderStats(stats, { transactions: [], total: 0, hasMore: false, append: false });
            });
        }

        // 2. 获取当前父分类下的流水
        statsOffset = 0;
        var txPromise = loadCategoryTransactions(false);

        return Promise.all([statsPromise, txPromise]).then(function(results) {
            renderStats(results[0], results[1]);
        });
    }

    function loadCategoryTransactions(append) {
        var filter = getFilterData();
        var offset = append ? statsOffset : 0;

        return send('ledger.transaction.list', {
            ledgerId: filter.ledgerId,
            dateFrom: filter.dateFrom,
            dateTo: filter.dateTo,
            categoryId: 0,
            type: '',
            parentCategoryId: currentParentCategoryId,
            offset: offset,
            limit: 30,
            sortBy: currentSortBy,
            sortOrder: currentSortOrder
        }).then(function(result) {
            var txs = result.transactions || [];
            var total = Number(result.total || 0);

            if (!append) {
                statsTotalTransactions = total;
                statsOffset = txs.length;
            } else {
                statsOffset += txs.length;
            }

            // 判断是否还有更多（减去已加载的）
            var loadedCount = append ? statsOffset : txs.length;
            statsHasMore = total > loadedCount;

            return {
                transactions: txs,
                total: total,
                hasMore: statsHasMore,
                append: append
            };
        });
    }

    function renderStats(stats, txResult) {
        stats = stats || {};
        txResult = txResult || { transactions: [], total: 0, hasMore: false, append: false };

        // 更新摘要卡片（默认仅显示支出；收入/结余隐藏，后续可通过配置开启）
        var totalExpense = Number(stats.totalExpense || 0);
        dom.summaryExpense.textContent = money(totalExpense);
        dom.summaryIncome.textContent = money(Number(stats.totalIncome || 0));
        dom.summaryBalance.textContent = money(stats.balance || 0);
        dom.summaryIncomeCard.classList.add('hidden');
        dom.summaryBalanceCard.classList.add('hidden');

        // 更新路径面包屑
        renderPathBreadcrumb();

        // 渲染分类 + 流水混合列表
        var categories = stats.categories || [];
        var transactions = txResult.transactions || [];
        var isAppend = txResult.append;
        if (!isAppend) {
            statsTransactions = transactions.slice();
        } else if (transactions.length) {
            statsTransactions = statsTransactions.concat(transactions);
        }

        // 判断是否在子分类下钻状态
        var isDrilledDown = currentCategoryPath.length > 0;

        // 根节点只显示分类，不显示流水
        var showTransactions = isDrilledDown;

        if (!categories.length && (!showTransactions || !transactions.length)) {
            dom.categoryStats.className = 'stats-list empty';
            dom.categoryStats.textContent = isDrilledDown ? '该分类下暂无数据' : '暂无统计数据';
            if (dom.statsLoadMore) dom.statsLoadMore.classList.add('hidden');
            if (dom.statsEnd) dom.statsEnd.classList.add('hidden');
            return;
        }

        dom.categoryStats.className = 'stats-list';

        var hasMore = txResult.hasMore;

        if (!isAppend) {
            // 非追加模式：完全重新渲染
            var html = '';

            // 先渲染分类行
            var catMax = categories.reduce(function(m, item) { return Math.max(m, Number(item.amount || 0)); }, 0) || 1;
            categories.forEach(function(item) {
                var percent = Math.max(4, Math.round(Number(item.amount || 0) / catMax * 100));
                var cls = item.type === 'income' ? 'income' : 'expense';
                html += '<div class="stat-row category-clickable ' + cls + '" data-category-id="' + item.categoryId + '" data-category-name="' + escapeHtml(item.name) + '">' +
                    '<div class="stat-top"><strong>' + escapeHtml(item.name) + '</strong><span>' + money(item.amount) + ' · ' + item.count + '笔</span></div>' +
                    '<div class="bar"><i style="width:' + percent + '%"></i></div>' +
                    '</div>';
            });

            // 仅在下钻状态才渲染流水行
            if (showTransactions) {
                transactions.forEach(function(t) {
                    html += makeStatsTxRow(t, isDrilledDown);
                });
            }

            dom.categoryStats.innerHTML = html;
        } else {
            // 追加模式：仅在下钻状态追加流水行
            if (showTransactions) {
                var appendHtml = '';
                transactions.forEach(function(t) {
                    appendHtml += makeStatsTxRow(t, isDrilledDown);
                });
                if (dom.statsLoadMore) dom.statsLoadMore.classList.add('hidden');
                dom.categoryStats.insertAdjacentHTML('beforeend', appendHtml);
            }
        }

        // 更新加载更多/结束指示器（根节点不显示）
        if (!showTransactions) {
            if (dom.statsLoadMore) dom.statsLoadMore.classList.add('hidden');
            if (dom.statsEnd) dom.statsEnd.classList.add('hidden');
        } else if (hasMore) {
            if (dom.statsLoadMore) dom.statsLoadMore.classList.remove('hidden');
            if (dom.statsEnd) dom.statsEnd.classList.add('hidden');
        } else {
            if (dom.statsLoadMore) dom.statsLoadMore.classList.add('hidden');
            if (dom.statsEnd) dom.statsEnd.classList.remove('hidden');
        }

        // 绑定分类点击事件（下钻）
        dom.categoryStats.querySelectorAll('[data-category-id]').forEach(function(el) {
            // 避免重复绑定
            if (el._catClickBound) return;
            el._catClickBound = true;
            el.addEventListener('click', function() {
                var catId = Number(this.getAttribute('data-category-id'));
                var catName = this.getAttribute('data-category-name');
                if (!catId) return;
                drillIntoCategory(catId, catName);
            });
        });

        // 绑定流水点击事件（展开详情）与操作按钮（编辑/删除/语音解析）。
        bindStatsTxExpandEvents();
        bindTransactionActionEvents(dom.categoryStats);
        bindVoiceParseEvents(dom.categoryStats);

        // 设置滚动监听（增量加载）
        setupInfiniteScroll();
    }

    function drillIntoCategory(categoryId, categoryName) {
        if (!categoryId) return;
        // 将当前父分类压入路径栈
        currentCategoryPath.push({ id: currentParentCategoryId, name: categoryName });
        currentParentCategoryId = categoryId;
        statsOffset = 0;
        statsHasMore = false;
        isLoadingMore = false;
        statsTotalTransactions = 0;
        loadStats();
    }

    function goBack() {
        if (currentCategoryPath.length === 0) return;
        var prev = currentCategoryPath.pop();
        currentParentCategoryId = prev.id;
        statsOffset = 0;
        statsHasMore = false;
        isLoadingMore = false;
        statsTotalTransactions = 0;
        loadStats();
    }

    function toggleSort(sortBy) {
        if (currentSortBy === sortBy) {
            // 切换升降序
            currentSortOrder = currentSortOrder === 'desc' ? 'asc' : 'desc';
        } else {
            currentSortBy = sortBy;
            currentSortOrder = 'desc';
        }
        statsOffset = 0;
        statsHasMore = false;
        isLoadingMore = false;
        statsTotalTransactions = 0;
        loadStats();
    }

    // 绑定统计页流水行的点击展开事件
    function bindStatsTxExpandEvents() {
        if (!dom.categoryStats) return;
        dom.categoryStats.querySelectorAll('[data-transaction-id]').forEach(function(el) {
            // 避免重复绑定
            if (el._txExpandBound) return;
            el._txExpandBound = true;
            el.addEventListener('click', function(e) {
                if (e.target.closest('button')) return;
                var panel = this.querySelector('.transaction-detail-panel');
                if (!panel) return;
                var isHidden = panel.classList.contains('hidden');
                // 关闭其他所有展开的详情面板
                dom.categoryStats.querySelectorAll('.transaction-item.expanded').forEach(function(other) {
                    if (other !== el) {
                        other.classList.remove('expanded');
                        var otherPanel = other.querySelector('.transaction-detail-panel');
                        if (otherPanel) otherPanel.classList.add('hidden');
                    }
                });
                // 切换当前
                panel.classList.toggle('hidden', !isHidden);
                this.classList.toggle('expanded', isHidden);
            });
        });
    }

    // 生成统计页流水行 HTML：主行与详情行为严格复用添加流水页最近流水标准。
    function makeStatsTxRow(t, isDrilledDown) {
        var cls = t.type === 'income' ? 'income' : 'expense';
        var sign = t.type === 'income' ? '+' : '-';
        var isVoice = Number(t.isVoiceInput || 0) === 1;
        var amount = Number(t.amount || 0);
        var voiceStatus = isVoice ? (' · 语音录入' + (t.voiceParseStatus ? ' · ' + t.voiceParseStatus : '')) : '';
        var voiceError = isVoice && t.voiceParseError ? '<small class="form-message error">解析失败：' + escapeHtml(t.voiceParseError) + '</small>' : '';
        var voiceCanReparse = isVoice && String(t.voiceParseStatus || '').toLowerCase() !== 'pending' && String(t.voiceParseStatus || '').toLowerCase() !== 'processing';
        var parseButton = voiceCanReparse && !isVoiceParseCompleted(t) ? '<button type="button" class="secondary" data-voice-parse="' + t.id + '">解析</button>' : '';
        var detailRows = buildRecentTransactionDetailRows(t, sign, amount);
        if (editingTransactionId && Number(t.id || 0) === Number(editingTransactionId)) cls += ' editing';

        return '<article class="transaction-item ' + cls + ' transaction-item-expandable" data-transaction-id="' + t.id + '">' +
            '<div class="transaction-main">' +
            '<strong>' + escapeHtml(t.parentCategoryName ? t.parentCategoryName + ' / ' + t.categoryName : t.categoryName) + '</strong>' +
            '<span>' + escapeHtml(t.description || '无备注') + '</span>' +
            '<small>' + escapeHtml(t.date || '') + ' · ' + escapeHtml(t.createdBy || '') + escapeHtml(voiceStatus) + '</small>' +
            voiceError +
            '</div>' +
            '<div class="transaction-amount">' + sign + money(amount) + parseButton + '</div>' +
            '<div class="transaction-detail-panel hidden">' + detailRows + '</div>' +
            '</article>';
    }

    function setupInfiniteScroll() {
        var el = dom.categoryStats;
        if (!el) return;

        // 移除旧的监听器（通过替换元素或使用标志避免重复绑定）
        if (el._scrollHandler) {
            el.removeEventListener('scroll', el._scrollHandler);
        }

        el._scrollHandler = function() {
            if (!statsHasMore || isLoadingMore) return;
            if (el.scrollHeight - el.scrollTop - el.clientHeight < 80) {
                isLoadingMore = true;
                if (dom.statsLoadMore) {
                    dom.statsLoadMore.textContent = '加载中...';
                    dom.statsLoadMore.classList.remove('hidden');
                }
                loadCategoryTransactions(true).then(function(txResult) {
                    // 追加模式：仅追加流水行，不重置摘要卡片
                    var isAppend = txResult.append;
                    var transactions = txResult.transactions || [];
                    var hasMore = txResult.hasMore;
                    var isDrilledDown = currentCategoryPath.length > 0;

                    var appendHtml = '';
                    transactions.forEach(function(t) {
                        appendHtml += makeStatsTxRow(t, isDrilledDown);
                    });

                    if (dom.statsLoadMore) dom.statsLoadMore.classList.add('hidden');
                    dom.categoryStats.insertAdjacentHTML('beforeend', appendHtml);

                    statsTransactions = statsTransactions.concat(transactions);

                    // 为新追加的流水行绑定展开事件与操作按钮
                    bindStatsTxExpandEvents();
                    bindTransactionActionEvents(dom.categoryStats);
                    bindVoiceParseEvents(dom.categoryStats);

                    // 更新加载更多/结束指示器
                    if (hasMore) {
                        if (dom.statsLoadMore) dom.statsLoadMore.classList.remove('hidden');
                        if (dom.statsEnd) dom.statsEnd.classList.add('hidden');
                    } else {
                        if (dom.statsLoadMore) dom.statsLoadMore.classList.add('hidden');
                        if (dom.statsEnd) dom.statsEnd.classList.remove('hidden');
                    }

                    isLoadingMore = false;
                }).catch(function() {
                    isLoadingMore = false;
                    if (dom.statsLoadMore) dom.statsLoadMore.classList.add('hidden');
                });
            }
        };

        el.addEventListener('scroll', el._scrollHandler);
    }

    function setTransactionDetailsExpanded(expanded) {
        if (!dom.transactionDetails || !dom.btnToggleTransactionDetails) return;
        dom.transactionDetails.classList.toggle('collapsed', !expanded);
        dom.btnToggleTransactionDetails.setAttribute('aria-expanded', expanded ? 'true' : 'false');
        dom.btnToggleTransactionDetails.textContent = expanded ? '收起类型/日期/分类/备注' : '展开类型/日期/分类/备注';
    }

    function startEditTransaction(id) {
        id = Number(id || 0);
        var t = findTransactionById(id);
        if (!id || !t) {
            showToast('未找到要编辑的流水，请刷新后重试', true);
            return;
        }

        if (dom.ledgerPage && !dom.ledgerPage.classList.contains('active')) {
            switchPage('ledger');
        }

        editingTransactionId = id;
        dom.transactionFormTitle.textContent = '编辑流水 #' + id;
        dom.btnCancelEdit.classList.remove('hidden');
        dom.transactionId.value = id;
        dom.transactionType.value = t.type || 'expense';
        renderCategorySelects();
        dom.transactionCategory.value = String(t.categoryId || '');
        dom.transactionAmount.value = Number(t.amount || 0).toFixed(2);
        dom.transactionDate.value = t.date || today();
        dom.transactionDescription.value = t.description || '';
        if (dom.btnSaveTransaction) dom.btnSaveTransaction.textContent = '提交修改';
        setTransactionDetailsExpanded(true);
        renderTransactions();

        if (dom.transactionForm && typeof dom.transactionForm.scrollIntoView === 'function') {
            dom.transactionForm.scrollIntoView({ behavior: 'smooth', block: 'center' });
        }
        setTimeout(function() { dom.transactionAmount.focus(); }, 120);
    }

    function resetTransactionForm() {
        editingTransactionId = 0;
        dom.transactionFormTitle.textContent = '';
        dom.btnCancelEdit.classList.add('hidden');
        dom.transactionId.value = '';
        dom.transactionType.value = 'expense';
        renderCategorySelects();
        dom.transactionAmount.value = '';
        dom.transactionDate.value = today();
        dom.transactionDescription.value = '';
        if (dom.btnSaveTransaction) dom.btnSaveTransaction.textContent = '保存';
        setTransactionDetailsExpanded(false);
        renderTransactions();
    }

    function saveTransaction() {
        if (!currentGroupId) { showToast('请先选择账本', true); return; }
        var amount = Number(dom.transactionAmount.value || 0);
        var categoryId = Number(dom.transactionCategory.value || 0);
        var type = dom.transactionType.value || 'expense';
        if (amount <= 0 || !categoryId) {
            showToast('请填写金额并选择分类', true);
            return;
        }
        if (type !== 'expense' && type !== 'income') {
            showToast('请选择正确的收支类型', true);
            return;
        }
        if (editingTransactionId && Number(editingTransactionId) <= 0) {
            showToast('编辑流水状态异常，请取消后重试', true);
            return;
        }

        var data = {
            ledgerId: currentGroupId,
            categoryId: categoryId,
            amount: amount,
            type: type,
            description: dom.transactionDescription.value.trim(),
            date: dom.transactionDate.value || today()
        };
        var cmd = 'ledger.transaction.create';
        var wasEditing = !!editingTransactionId;
        if (wasEditing) {
            cmd = 'ledger.transaction.update';
            data.id = Number(editingTransactionId);
        }

        if (dom.btnSaveTransaction) dom.btnSaveTransaction.disabled = true;
        send(cmd, data).then(function() {
            showToast(wasEditing ? '流水已更新' : '流水已添加');
            resetTransactionForm();
            loadStats();
            loadTransactions();
            loadQuickCategoryStats();
        }).finally(function() {
            if (dom.btnSaveTransaction) dom.btnSaveTransaction.disabled = false;
        });
    }

    function switchSettingsSubPage(page) {
        page = page || 'groups';
        dom.settingsSubTabs.forEach(function(tab) {
            tab.classList.toggle('active', tab.getAttribute('data-settings-page') === page);
        });
        dom.settingsGroupsPage.classList.toggle('active', page === 'groups');
        dom.settingsCategoriesPage.classList.toggle('active', page === 'categories');
        dom.settingsMembersPage.classList.toggle('active', page === 'members');

        if (page === 'categories' && currentGroupId) loadCategories();
        if (page === 'members') loadMembers();
        if (page === 'groups') {
            updateVoiceTestSectionVisibility();
            loadPidBindings();
        }
        scheduleCategoryListHeightUpdate();
    }

    function switchPage(page) {
        dom.tabLedger.classList.toggle('active', page === 'ledger');
        dom.tabStats.classList.toggle('active', page === 'stats');
        dom.tabSettings.classList.toggle('active', page === 'settings');
        dom.ledgerPage.classList.toggle('active', page === 'ledger');
        dom.statsPage.classList.toggle('active', page === 'stats');
        dom.settingsPage.classList.toggle('active', page === 'settings');
        if (page === 'settings') {
            switchSettingsSubPage('groups');
        }
        scheduleAllAdaptiveListHeightUpdates();
    }

    function bindEvents() {
        if (dom.inviteLink) {
            dom.inviteLink.addEventListener('keydown', function(e) {
                var isCopyShortcut = (e.ctrlKey || e.metaKey) && (e.key === 'c' || e.key === 'C' || e.key === 'a' || e.key === 'A');
                var isNavigation = ['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', 'Home', 'End', 'Tab'].indexOf(e.key) >= 0;
                if (!isCopyShortcut && !isNavigation) e.preventDefault();
            });
            dom.inviteLink.addEventListener('paste', function(e) { e.preventDefault(); });
        }
        if (dom.btnCopyInvite) {
            dom.btnCopyInvite.addEventListener('click', copyInviteLink);
        }
        dom.loginUsername.addEventListener('blur', precheckLoginUsername);
        dom.loginUsername.addEventListener('input', function() {
            resetLoginPrecheck(dom.loginUsername.value.trim());
        });
        dom.loginForm.addEventListener('submit', function(e) {
            e.preventDefault();
            login(dom.loginUsername.value.trim(), dom.loginPassword.value);
        });
        dom.btnLogout.addEventListener('click', logout);
        dom.btnRefresh.addEventListener('click', function() { refreshCurrentPage(false); });
        if (dom.btnToggleGroupForm) {
            dom.btnToggleGroupForm.addEventListener('click', function() {
                var isVisible = dom.groupForm && !dom.groupForm.classList.contains('hidden');
                setGroupCreateFormVisible(!isVisible, !isVisible);
            });
        }
        dom.authUser.addEventListener('click', function() { refreshCurrentPage(false); });
        dom.authUser.addEventListener('keydown', function(e) {
            if (e.key !== 'Enter' && e.key !== ' ') return;
            e.preventDefault();
            refreshCurrentPage(false);
        });
        document.addEventListener('visibilitychange', handleVisibilityChange);
        dom.groupForm.addEventListener('submit', function(e) {
            e.preventDefault();
            var name = dom.groupName.value.trim();
            if (!name) return showToast('请输入账本名称', true);
            send('ledger.group.create', { name: name }).then(function(data) {
                dom.groupName.value = '';
                currentGroupId = Number(data.id || 0);
                setGroupCreateFormVisible(false, false);
                showToast('账本已创建');
                loadGroups();
            });
        });
        if (dom.billImportForm) {
            dom.billImportForm.addEventListener('submit', function(e) {
                e.preventDefault();
                uploadBillImport();
            });
        }
        dom.memberCreateFamilyForm.addEventListener('submit', function(e) {
            e.preventDefault();
            createFamilyGroupFromMembersPage();
        });
        dom.btnPeriodWeek.addEventListener('click', function() { setStatsPeriod('week', new Date(), true); });
        dom.btnPeriodMonth.addEventListener('click', function() { setStatsPeriod('month', new Date(), true); });
        dom.btnPeriodYear.addEventListener('click', function() { setStatsPeriod('year', new Date(), true); });
        dom.btnPrevPeriod.addEventListener('click', function() { shiftStatsPeriod(-1); });
        dom.btnNextPeriod.addEventListener('click', function() { shiftStatsPeriod(1); });
        dom.transactionType.addEventListener('change', renderCategorySelects);
        dom.transactionCategory.addEventListener('change', renderQuickCategories);
        dom.btnToggleTransactionDetails.addEventListener('click', function() {
            setTransactionDetailsExpanded(dom.transactionDetails.classList.contains('collapsed'));
        });
        dom.tabLedger.addEventListener('click', function() { switchPage('ledger'); });
        dom.tabStats.addEventListener('click', function() { switchPage('stats'); });
        dom.tabSettings.addEventListener('click', function() { switchPage('settings'); });
        dom.settingsSubTabs.forEach(function(tab) {
            tab.addEventListener('click', function() {
                switchSettingsSubPage(this.getAttribute('data-settings-page'));
            });
        });
        dom.transactionForm.addEventListener('submit', function(e) {
            e.preventDefault();
            saveTransaction();
        });
        dom.btnCancelEdit.addEventListener('click', resetTransactionForm);
        dom.btnPrevPage.addEventListener('click', function() {
            if (currentPage > 1) { currentPage--; loadTransactions(); }
        });
        dom.btnNextPage.addEventListener('click', function() {
            var maxPage = Math.max(1, Math.ceil(totalTransactions / PAGE_SIZE));
            if (currentPage < maxPage) { currentPage++; loadTransactions(); }
        });
        dom.categoryType.addEventListener('change', renderCategoryParentOptions);
        if (dom.btnToggleCategoryForm) {
            dom.btnToggleCategoryForm.addEventListener('click', function() {
                var expanded = dom.categoryFormPanel && !dom.categoryFormPanel.classList.contains('hidden');
                if (expanded) {
                    resetCategoryForm();
                    return;
                }
                dom.btnSaveCategory.textContent = '添加';
                dom.btnCancelCategoryEdit.classList.add('hidden');
                setCategoryFormExpanded(true);
                dom.categoryName.focus();
            });
        }
        dom.btnCancelCategoryEdit.addEventListener('click', resetCategoryForm);
        dom.categoryForm.addEventListener('submit', function(e) {
            e.preventDefault();
            var name = dom.categoryName.value.trim();
            var editingId = Number(dom.categoryId.value || 0);
            var isExpanded = !dom.categoryFormPanel || !dom.categoryFormPanel.classList.contains('hidden');
            if (!isExpanded) {
                setCategoryFormExpanded(true);
                dom.categoryName.focus();
                return;
            }
            var cmd = editingId ? 'ledger.category.update' : 'ledger.category.create';
            var data = {
                ledgerId: currentGroupId,
                name: name,
                type: dom.categoryType.value,
                parentId: Number(dom.categoryParent.value || 0),
                sortOrder: Number(dom.categorySort.value || 0)
            };
            if (editingId) data.id = editingId;
            if (!currentGroupId) return showToast('请先选择账本', true);
            if (!name) return showToast('请输入分类名称', true);
            send(cmd, data).then(function() {
                showToast(editingId ? '分类已更新' : '分类已添加');
                resetCategoryForm();
                loadCategories().then(function() { loadStats(); loadTransactions(); loadQuickCategoryStats(); });
            });
        });
        dom.memberForm.addEventListener('submit', function(e) {
            e.preventDefault();
            var username = dom.memberUsername.value.trim();
            if (!currentGroupId) return showToast('请先选择账本', true);
            if (!username) return showToast('请输入用户名', true);
            send('ledger.group.invite_member', {
                groupId: currentGroupId,
                username: username,
                role: dom.memberRole.value
            }).then(function() {
                dom.memberUsername.value = '';
                showToast('邀请已发送，等待对方确认');
                loadMembers();
            });
        });
        if (dom.inviteForm) {
            dom.inviteForm.addEventListener('submit', function(e) {
                e.preventDefault();
                createInviteRegistration();
            });
        }
        if (dom.inviteJoinLedger) {
            dom.inviteJoinLedger.addEventListener('change', loadCurrentInviteRegistration);
        }
        if (dom.inviteFamilyRole) {
            dom.inviteFamilyRole.addEventListener('change', loadCurrentInviteRegistration);
        }
        dom.pidForm.addEventListener('submit', function(e) {
            e.preventDefault();
            createPidBinding();
        });
        dom.voiceTestForm.addEventListener('submit', function(e) {
            e.preventDefault();
            var text = dom.voiceTestText.value.trim();
            if (!text) { showToast('请输入口语文本', true); return; }
            if (!currentGroupId) { showToast('请先选择账本', true); return; }

            dom.btnVoiceTest.disabled = true;
            dom.btnVoiceTest.textContent = '录入中...';
            dom.voiceTestResult.classList.add('hidden');

            send('ledger.voice.test', { text: text, ledgerId: currentGroupId })
                .then(function(data) {
                    renderVoiceTestResult(data);
                    dom.voiceTestText.value = '';
                    showToast('语音文本已录入，等待后台解析');
                    startVoiceAttentionPoll(true);
                    loadStats();
                    loadTransactions();
                    loadQuickCategoryStats();
                })
                .catch(function(err) {
                    dom.voiceTestResult.innerHTML = '<div class="form-message error">' + escapeHtml(err.message || '录入失败') + '</div>';
                    dom.voiceTestResult.classList.remove('hidden');
                })
                .finally(function() {
                    dom.btnVoiceTest.disabled = false;
                    dom.btnVoiceTest.textContent = '录入语音文本账目';
                });
        });

        // ---- 统计分页新元素事件绑定 ----
        if (dom.btnSortAmount) {
            dom.btnSortAmount.addEventListener('click', function() { toggleSort('amount'); });
        }
        if (dom.btnSortTime) {
            dom.btnSortTime.addEventListener('click', function() { toggleSort('time'); });
        }
        if (dom.btnStatsBack) {
            dom.btnStatsBack.addEventListener('click', goBack);
        }
        window.addEventListener('resize', scheduleAllAdaptiveListHeightUpdates);
    }

    bindEvents();
    dom.transactionDate.value = today();
    setTransactionDetailsExpanded(false);
    setStatsPeriod('month', new Date(), false);

    if (loadAuth()) {
        updateAdminEntryVisibility();
        checkSession().then(function(valid) {
            if (valid) {
                updateAdminEntryVisibility();
                showApp();
                scheduleRecentTransactionListHeightUpdate();
                initializeLedger();
            } else {
                clearAuth();
                showLogin();
            }
        });
    } else {
        showLogin();
    }
})();
