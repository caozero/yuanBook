(function() {
    'use strict';
    console.log('[admin.js source ../www/js/admin.js marker-20260707-0209]');

    var STORAGE_KEY_TOKEN = 'cp_home_ledger_token';
    var STORAGE_KEY_USERNAME = 'cp_home_ledger_username';
    var STORAGE_KEY_PERMISSIONS = 'cp_home_ledger_permissions';
    var VALUE_TYPE_OPTIONS = ['string', 'int', 'double', 'bool'];

    var authToken = localStorage.getItem(STORAGE_KEY_TOKEN) || '';
    var authUsername = localStorage.getItem(STORAGE_KEY_USERNAME) || '';
    var authPermissions = localStorage.getItem(STORAGE_KEY_PERMISSIONS) || '[]';
    var settingsCache = [];
    var groupedSettingsCache = [];
    var activeSettingsGroupKey = '';
    var usersCache = [];
    var usersLoaded = false;
    var donationImageUrl = '';
    var donationImageLoading = null;

    function createExpandableListState() {
        var expandedKeys = new Set();
        return {
            isExpanded: function(key) {
                return expandedKeys.has(String(key || ''));
            },
            toggle: function(key) {
                var normalizedKey = String(key || '');
                if (!normalizedKey) return false;
                if (expandedKeys.has(normalizedKey)) {
                    expandedKeys.delete(normalizedKey);
                    return false;
                }
                expandedKeys.add(normalizedKey);
                return true;
            },
            prune: function(validKeys) {
                var validKeySet = new Set((validKeys || []).map(function(key) {
                    return String(key || '');
                }));
                expandedKeys.forEach(function(key) {
                    if (!validKeySet.has(key)) expandedKeys.delete(key);
                });
            }
        };
    }

    var userExpansionState = createExpandableListState();

    var dom = {
        currentUser: document.getElementById('admin-current-user'),
        listMessage: document.getElementById('admin-list-message'),
        settingsBody: document.getElementById('admin-settings-body'),
        settingsPagination: document.getElementById('admin-settings-pagination'),
        settingsGroupTitle: document.getElementById('admin-settings-group-title'),
        logoutButton: document.getElementById('btn-admin-logout'),
        domainTabs: document.querySelectorAll('.admin-domain-tab'),
        settingsDomain: document.getElementById('admin-settings-domain'),
        usersDomain: document.getElementById('admin-users-domain'),
        donationDomain: document.getElementById('admin-donation-domain'),
        donationImage: document.getElementById('admin-donation-image'),
        donationStatus: document.getElementById('admin-donation-status'),
        usersMessage: document.getElementById('admin-users-message'),
        usersBody: document.getElementById('admin-users-body'),
        createUserButton: document.getElementById('btn-create-user'),
        createUserForm: document.getElementById('admin-user-create-form'),
        newUsername: document.getElementById('admin-new-username'),
        newPassword: document.getElementById('admin-new-password'),
        newIsAdmin: document.getElementById('admin-new-is-admin'),
        submitUserButton: document.getElementById('btn-submit-user'),
        cancelUserButton: document.getElementById('btn-cancel-user'),
        independentInviteLink: document.getElementById('admin-independent-invite-link'),
        independentInviteStatus: document.getElementById('admin-independent-invite-status'),
        createIndependentInviteButton: document.getElementById('btn-create-independent-invite'),
        copyIndependentInviteButton: document.getElementById('btn-copy-independent-invite')
    };
//AI不要修改这段,开始
    function escapeHtml(value) {
        return String(value == null ? '' : value)
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;')
            .replace(/"/g, '&quot;')
            .replace(/'/g, '&#39;'); // 语法修复：使用了单引号的 HTML 实体包裹在单引号中
    }
//AI不要修改这段,结束
    
    function escapeAttribute(value) {
        return escapeHtml(value).replace(/`/g, '&#96;');
    }

    function parsePermissions(raw) {
        if (!raw) return [];
        if (Array.isArray(raw)) return raw;
        try {
            var parsed = JSON.parse(raw);
            return Array.isArray(parsed) ? parsed : [];
        } catch (err) {
            return [];
        }
    }

    function hasAdminPermission() {
        var permissions = parsePermissions(authPermissions);
        return permissions.indexOf('admin') >= 0;
    }

    function setMessage(target, text, isError) {
        if (!target) return;
        target.textContent = text || '';
        target.className = 'form-message' + (text ? '' : ' hidden') + (isError ? ' error' : ' success');
    }

    function setListMessage(text, isError) {
        if (!dom.listMessage) return;
        dom.listMessage.textContent = text || '';
        dom.listMessage.className = isError ? 'form-message error' : 'muted';
    }

    function logout() {
        return fetch('/api/logout', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ token: authToken })
        }).finally(function() {
            localStorage.removeItem(STORAGE_KEY_TOKEN);
            localStorage.removeItem(STORAGE_KEY_USERNAME);
            localStorage.removeItem(STORAGE_KEY_PERMISSIONS);
            window.location.href = '/ledger.html';
        });
    }

    function send(cmd, data) {
        return fetch('/api/channel', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                cmd: cmd,
                token: authToken,
                data: data || {}
            })
        }).then(function(res) {
            return res.json().then(function(payload) {
                if (!res.ok) {
                    throw new Error(payload && payload.data && payload.data.message ? payload.data.message : '请求失败');
                }
                if (payload && payload.data && payload.data.error) {
                    throw new Error(payload.data.error);
                }
                return payload ? payload.data : null;
            });
        });
    }

    function formatTime(value) {
        if (!value) return '-';
        if (/^\d+$/.test(String(value))) {
            return new Date(Number(value) * 1000).toLocaleString();
        }
        var date = new Date(value);
        if (!isNaN(date.getTime())) {
            return date.toLocaleString();
        }
        return String(value);
    }

    function buildSettingDescription(item) {
        return item && item.description ? String(item.description) : '';
    }

    function isBoolSetting(item) {
        return item && String(item.valueType || '').toLowerCase() === 'bool';
    }

    function normalizeBoolSettingValue(value) {
        var normalized = String(value == null ? '' : value).trim().toLowerCase();
        return (normalized === 'true' || normalized === '1' || normalized === 'yes' || normalized === 'on') ? 'true' : 'false';
    }

    function buildSettingValueEditor(item) {
        var rawValue = item && item.settingValue != null ? item.settingValue : '';
        var sensitivePlaceholder = item && item.isSensitive ? ' placeholder="敏感值"' : '';

        if (isBoolSetting(item)) {
            var boolValue = normalizeBoolSettingValue(rawValue);
            return [
                '<label class="admin-inline-checkbox">',
                '<input data-field="value" type="checkbox" value="true"' + (boolValue === 'true' ? ' checked' : '') + '>',
                '<span>启用</span>',
                '</label>'
            ].join('');
        }

        return '<input class="admin-inline-input" data-field="value" type="text" value="' + escapeAttribute(rawValue) + '"' + sensitivePlaceholder + '>';
    }

    function normalizeSettingsGroupKey(category) {
        var text = category == null ? '' : String(category).trim();
        return text || '__ungrouped__';
    }

    function getSettingsGroupLabel(category) {
        var text = category == null ? '' : String(category).trim();
        return text || '未分组';
    }

    function getSettingsGroupDisplayLabel(category) {
        var rawLabel = getSettingsGroupLabel(category);
        var normalizedLabel = rawLabel.toLowerCase();
        var labelMap = {
            '__ungrouped__': '未分组',
            'system': '系统',
            'security': '安全',
            'auth': '认证',
            'account': '账户',
            'ledger': '记账',
            'family': '家庭',
            'voice': '语音',
            'import': '导入',
            'notification': '通知',
            'storage': '存储',
            'other': '其他'
        };

        return labelMap[normalizedLabel] || rawLabel;
    }

    function buildGroupedSettings(items) {
        var groupMap = {};
        var orderedGroupKeys = [];

        (items || []).forEach(function(item) {
            var rawCategory = item && item.category ? item.category : '';
            var groupKey = normalizeSettingsGroupKey(rawCategory);
            if (!groupMap[groupKey]) {
                groupMap[groupKey] = {
                    key: groupKey,
                    label: getSettingsGroupDisplayLabel(rawCategory),
                    items: []
                };
                orderedGroupKeys.push(groupKey);
            }
            groupMap[groupKey].items.push(item);
        });

        return orderedGroupKeys.map(function(groupKey) {
            return groupMap[groupKey];
        });
    }

    function getActiveSettingsGroup() {
        if (!groupedSettingsCache.length) {
            return null;
        }

        var matchedGroup = groupedSettingsCache.find(function(group) {
            return group.key === activeSettingsGroupKey;
        });

        return matchedGroup || groupedSettingsCache[0];
    }

    function ensureActiveSettingsGroup() {
        var group = getActiveSettingsGroup();
        activeSettingsGroupKey = group ? group.key : '';
        return group;
    }

    function renderSettingsPagination() {
        if (!dom.settingsPagination) return;

        if (!groupedSettingsCache.length) {
            dom.settingsPagination.innerHTML = '';
            return;
        }

        var html = groupedSettingsCache.map(function(group) {
            var isActive = group.key === activeSettingsGroupKey;
            return [
                '<button type="button" class="admin-page-button' + (isActive ? ' active' : '') + '" data-action="switch-group" data-group-key="' + escapeAttribute(group.key) + '">',
                '<span class="admin-page-button-text">' + escapeHtml(group.label) + '</span>',
                '<span class="admin-page-button-count">' + escapeHtml(group.items.length) + '</span>',
                '</button>'
            ].join('');
        }).join('');

        dom.settingsPagination.innerHTML = html;
    }

    function renderSettingsGroupTitle(group) {
        if (!dom.settingsGroupTitle) return;
        if (!group) {
            dom.settingsGroupTitle.textContent = '全部参数';
            return;
        }
        dom.settingsGroupTitle.textContent = group.label + '（' + group.items.length + '）';
    }

    function renderSettingsRows(items) {
        if (!items.length) {
            dom.settingsBody.innerHTML = '<li class="admin-empty-cell">当前分组暂无系统参数</li>';
            return;
        }

        var html = items.map(function(item) {
            var settingKey = item.settingKey || '';
            var descriptionText = buildSettingDescription(item);
            return [
                '<li class="admin-setting-item" data-key="' + escapeAttribute(settingKey) + '">',
                '<div class="admin-setting-description">' + escapeHtml(descriptionText || settingKey) + '</div>',
                '<div class="admin-setting-editor-row">',
                buildSettingValueEditor(item),
                '<button type="button" class="admin-table-button" data-action="save" data-key="' + escapeAttribute(settingKey) + '">提交</button>',
                '</div>',
                '</li>'
            ].join('');
        }).join('');

        dom.settingsBody.innerHTML = html;
    }

    function renderSettings() {
        groupedSettingsCache = buildGroupedSettings(settingsCache);

        if (!groupedSettingsCache.length) {
            renderSettingsGroupTitle(null);
            renderSettingsPagination();
            dom.settingsBody.innerHTML = '<li class="admin-empty-cell">暂无系统参数</li>';
            return;
        }

        var activeGroup = ensureActiveSettingsGroup();
        renderSettingsPagination();
        renderSettingsGroupTitle(activeGroup);
        renderSettingsRows(activeGroup ? activeGroup.items : []);
    }

    function switchSettingsGroup(groupKey) {
        activeSettingsGroupKey = groupKey || '';
        renderSettings();
    }

    function setUsersMessage(text, isError) {
        if (!dom.usersMessage) return;
        dom.usersMessage.textContent = text || '';
        dom.usersMessage.className = isError ? 'form-message error' : 'muted';
    }

    function userIsAdmin(user) {
        return parsePermissions(user && user.permissions).indexOf('admin') >= 0;
    }

    function renderUserActions(user, username, isSelf, isActive) {
        var actions = [];
        if (isSelf) {
            actions.push('<span class="admin-user-self-badge">当前账号</span>');
        } else {
            actions.push('<button type="button" class="admin-table-button" data-user-action="toggle" data-username="' + escapeAttribute(username) + '" data-active="' + (isActive ? '0' : '1') + '">' + (isActive ? '禁用' : '启用') + '</button>');
            if (isActive) {
                actions.push('<button type="button" class="admin-table-button" data-user-action="password" data-username="' + escapeAttribute(username) + '">设置密码</button>');
                actions.push('<button type="button" class="admin-secondary-button" data-user-action="clear-password" data-username="' + escapeAttribute(username) + '">清空密码</button>');
            }
            actions.push('<button type="button" class="admin-table-button danger" data-user-action="delete" data-username="' + escapeAttribute(username) + '">删除</button>');
        }
        return actions.join('');
    }

    function renderUsers() {
        if (!dom.usersBody) return;
        if (!usersCache.length) {
            userExpansionState.prune([]);
            dom.usersBody.innerHTML = '<li class="admin-empty-cell">暂无用户</li>';
            return;
        }

        userExpansionState.prune(usersCache.map(function(user) {
            return user && user.username ? user.username : '';
        }));

        dom.usersBody.innerHTML = usersCache.map(function(user, index) {
            var username = user.username || '';
            var isSelf = username === authUsername;
            var isActive = Number(user.isActive) !== 0;
            var isExpanded = userExpansionState.isExpanded(username);
            var detailId = 'admin-user-detail-' + index;
            return [
                '<li class="admin-setting-item admin-user-item' + (isExpanded ? ' expanded' : '') + '" data-username="' + escapeAttribute(username) + '">',
                '<button type="button" class="admin-user-summary-toggle" data-user-expand="' + escapeAttribute(username) + '" aria-expanded="' + (isExpanded ? 'true' : 'false') + '" aria-controls="' + detailId + '">',
                '<span class="admin-user-summary">',
                '<strong>' + escapeHtml(username) + '</strong>',
                '<span class="admin-user-state ' + (isActive ? 'active' : 'disabled') + '">' + (isActive ? '启用' : '已停用') + '</span>',
                '<span class="admin-user-role">' + (userIsAdmin(user) ? '管理员' : '普通用户') + '</span>',
                '</span>',
                '<span class="admin-user-expand-indicator" aria-hidden="true">⌄</span>',
                '</button>',
                '<div id="' + detailId + '" class="admin-user-detail"' + (isExpanded ? '' : ' hidden') + '>',
                '<div class="admin-user-detail-fields">',
                '<span><small>密码状态</small><strong>' + (user.hasPassword ? '已设置密码' : '待初始化密码') + '</strong></span>',
                '<span><small>创建时间</small><strong>' + escapeHtml(formatTime(user.createdAt)) + '</strong></span>',
                '<span><small>最近登录</small><strong>' + escapeHtml(formatTime(user.lastLogin)) + '</strong></span>',
                '</div>',
                '<div class="admin-user-actions">' + renderUserActions(user, username, isSelf, isActive) + '</div>',
                '</div>',
                '</li>'
            ].join('');
        }).join('');
    }

    function buildInviteLink(code) {
        var params = new URLSearchParams();
        params.set('code', code || '');
        return window.location.origin + '/register.html?' + params.toString();
    }

    function formatInviteExpiresAt(expiresAt) {
        var timestamp = Number(expiresAt || 0);
        if (timestamp <= 0) return '';
        var date = new Date(timestamp * 1000);
        return isNaN(date.getTime()) ? '' : date.toLocaleString();
    }

    function setIndependentInviteState(invite, message) {
        var hasInvite = !!(invite && invite.code);
        if (dom.independentInviteLink) {
            dom.independentInviteLink.value = hasInvite ? buildInviteLink(invite.code) : '';
        }
        if (dom.independentInviteStatus) {
            var expiresText = hasInvite ? formatInviteExpiresAt(invite.expiresAt) : '';
            dom.independentInviteStatus.textContent = message || (hasInvite
                ? '当前有效邀请可直接复制' + (expiresText ? '，有效期至：' + expiresText : '') + '。'
                : '当前管理员没有可复用的有效独立用户邀请。');
        }
    }

    function loadIndependentInvite() {
        if (!dom.independentInviteLink) return Promise.resolve();
        if (dom.independentInviteStatus) dom.independentInviteStatus.textContent = '正在查询有效邀请...';
        return send('ledger.invite.current', {
            inviteEntry: 'admin_user_management',
            inviteType: 'independent_user'
        })
            .then(function(data) {
                setIndependentInviteState(data && data.hasInvite ? data : null);
            })
            .catch(function(err) {
                setIndependentInviteState(null, err.message || '查询独立用户邀请失败');
            });
    }

    function createIndependentInvite() {
        if (!dom.createIndependentInviteButton) return Promise.resolve();
        dom.createIndependentInviteButton.disabled = true;
        if (dom.copyIndependentInviteButton) dom.copyIndependentInviteButton.disabled = true;
        if (dom.independentInviteStatus) dom.independentInviteStatus.textContent = '正在生成独立用户邀请...';
        return send('ledger.invite.create', {
            inviteEntry: 'admin_user_management',
            inviteType: 'independent_user'
        })
            .then(function(data) {
                if (!data || data.inviteType !== 'independent_user' || Number(data.familyId || 0) !== 0 || Number(data.defaultLedgerId || 0) !== 0) {
                    throw new Error('服务端返回的邀请作用域不是独立用户，已拒绝展示该链接');
                }
                setIndependentInviteState(data, '独立用户邀请已生成；注册后不会加入任何家庭或账本。');
                return copyIndependentInvite();
            })
            .catch(function(err) {
                setIndependentInviteState(null, err.message || '独立用户邀请生成失败');
            })
            .finally(function() {
                dom.createIndependentInviteButton.disabled = false;
                if (dom.copyIndependentInviteButton) dom.copyIndependentInviteButton.disabled = false;
            });
    }

    function copyIndependentInvite() {
        if (!dom.independentInviteLink || !dom.independentInviteLink.value) {
            setIndependentInviteState(null, '请先生成独立用户邀请。');
            return Promise.resolve();
        }
        var link = dom.independentInviteLink.value;
        if (navigator.clipboard && navigator.clipboard.writeText) {
            return navigator.clipboard.writeText(link).catch(function() {
                dom.independentInviteLink.focus();
                dom.independentInviteLink.select();
            });
        }
        dom.independentInviteLink.focus();
        dom.independentInviteLink.select();
        return Promise.resolve();
    }

    function loadDonationImage() {
        if (!dom.donationImage || !dom.donationStatus) return Promise.resolve();
        if (donationImageUrl) {
            dom.donationImage.src = donationImageUrl;
            dom.donationImage.classList.remove('hidden');
            dom.donationStatus.classList.add('hidden');
            return Promise.resolve();
        }
        if (donationImageLoading) return donationImageLoading;

        dom.donationImage.classList.add('hidden');
        dom.donationStatus.className = 'muted';
        dom.donationStatus.textContent = '正在加载赞赏码...';

        donationImageLoading = fetch('/api/donation/image', {
            headers: { 'Authorization': 'Bearer ' + authToken },
            cache: 'force-cache'
        }).then(function(res) {
            if (!res.ok) {
                return res.json().catch(function() { return null; }).then(function(payload) {
                    throw new Error(payload && payload.message ? payload.message : '赞赏码加载失败');
                });
            }
            return res.blob();
        }).then(function(blob) {
            if (!blob || !blob.size) throw new Error('赞赏码数据为空');
            donationImageUrl = URL.createObjectURL(blob);
            dom.donationImage.src = donationImageUrl;
            dom.donationImage.classList.remove('hidden');
            dom.donationStatus.classList.add('hidden');
        }).catch(function(err) {
            dom.donationImage.classList.add('hidden');
            dom.donationStatus.className = 'form-message error';
            dom.donationStatus.textContent = err.message || '赞赏码加载失败';
        }).finally(function() {
            donationImageLoading = null;
        });

        return donationImageLoading;
    }

    function loadUsers() {
        setUsersMessage('正在加载用户...', false);
        return send('ledger.user.list', {}).then(function(data) {
            usersCache = data && data.users ? data.users : [];
            usersLoaded = true;
            renderUsers();
            setUsersMessage('共 ' + usersCache.length + ' 个用户', false);
        }).catch(function(err) {
            usersCache = [];
            renderUsers();
            setUsersMessage(err.message || '加载用户失败', true);
        });
    }

    function switchDomain(domain) {
        var showSettings = domain === 'settings';
        var showUsers = domain === 'users';
        var showDonation = domain === 'donation';
        dom.settingsDomain.classList.toggle('hidden', !showSettings);
        dom.usersDomain.classList.toggle('hidden', !showUsers);
        if (dom.donationDomain) dom.donationDomain.classList.toggle('hidden', !showDonation);
        Array.prototype.forEach.call(dom.domainTabs, function(tab) {
            tab.classList.toggle('active', tab.getAttribute('data-domain') === domain);
        });
        if (showUsers) {
            loadIndependentInvite();
            if (!usersLoaded) loadUsers();
        }
        if (showDonation) {
            loadDonationImage();
        }
    }

    function loadSettings() {
        setListMessage('正在加载...', false);
        return send('ledger.settings.list', {})
            .then(function(data) {
                settingsCache = data && data.settings ? data.settings : [];
                ensureActiveSettingsGroup();
                renderSettings();
                setListMessage('', false);
            })
            .catch(function(err) {
                settingsCache = [];
                renderSettings();
                setListMessage(err.message || '加载系统参数失败', true);
            });
    }

    function ensureAdminSession() {
        if (!authToken) {
            window.location.href = '/ledger.html';
            return Promise.reject(new Error('未登录'));
        }

        return fetch('/api/session?token=' + encodeURIComponent(authToken))
            .then(function(res) {
                return res.json().then(function(data) {
                    if (!res.ok || !data.ok || !data.username) {
                        throw new Error('登录状态已失效');
                    }
                    authToken = data.token || authToken;
                    authUsername = data.username || '';
                    authPermissions = JSON.stringify(data.permissions || []);
                    localStorage.setItem(STORAGE_KEY_TOKEN, authToken);
                    localStorage.setItem(STORAGE_KEY_USERNAME, authUsername);
                    localStorage.setItem(STORAGE_KEY_PERMISSIONS, authPermissions);
                    if (!hasAdminPermission()) {
                        throw new Error('当前用户不是管理员');
                    }
                    dom.currentUser.textContent = authUsername || '-';
                    return true;
                });
            })
            .catch(function(err) {
                setListMessage(err.message || '管理员认证失败', true);
                setTimeout(function() {
                    window.location.href = '/ledger.html';
                }, 1200);
                throw err;
            });
    }

    function getRowBySettingKey(settingKey) {
        if (!settingKey || !dom.settingsBody) return null;
        return dom.settingsBody.querySelector('li[data-key="' + CSS.escape(settingKey) + '"]');
    }

    function collectRowPayload(settingKey) {
        var row = getRowBySettingKey(settingKey);
        if (!row) {
            throw new Error('未找到要提交的参数行');
        }

        var valueInput = row.querySelector('[data-field="value"]');
        var originalItem = settingsCache.find(function(item) {
            return (item.settingKey || '') === settingKey;
        }) || {};
        var value = valueInput && valueInput.type === 'checkbox'
            ? (valueInput.checked ? 'true' : 'false')
            : (valueInput ? valueInput.value : '');

        return {
            settingKey: settingKey,
            value: value,
            valueType: originalItem.valueType || 'string',
            category: originalItem.category || '',
            isSensitive: !!originalItem.isSensitive,
            description: originalItem.description || ''
        };
    }

    function saveSetting(settingKey) {
        if (!settingKey) {
            setListMessage('参数键不能为空。', true);
            return Promise.resolve();
        }

        var payload;
        try {
            payload = collectRowPayload(settingKey);
        } catch (err) {
            setListMessage(err.message || '读取参数行失败', true);
            return Promise.resolve();
        }

        setListMessage('正在提交 ' + settingKey + ' ...', false);
        return send('ledger.settings.update', payload)
            .then(function() {
                return loadSettings();
            })
            .catch(function(err) {
                setListMessage(err.message || '保存系统参数失败', true);
            });
    }

    Array.prototype.forEach.call(dom.domainTabs, function(tab) {
        tab.addEventListener('click', function() {
            switchDomain(tab.getAttribute('data-domain') || 'settings');
        });
    });

    dom.createUserButton.addEventListener('click', function() {
        dom.createUserForm.classList.remove('hidden');
        dom.newUsername.focus();
    });

    dom.cancelUserButton.addEventListener('click', function() {
        dom.createUserForm.classList.add('hidden');
    });

    dom.submitUserButton.addEventListener('click', function() {
        var username = dom.newUsername.value.trim();
        if (!username) {
            setUsersMessage('用户名不能为空', true);
            return;
        }
        dom.submitUserButton.disabled = true;
        send('ledger.user.create', {
            username: username,
            password: dom.newPassword.value,
            permissions: dom.newIsAdmin.checked ? ['user', 'admin'] : ['user']
        }).then(function() {
            dom.newUsername.value = '';
            dom.newPassword.value = '';
            dom.newIsAdmin.checked = false;
            dom.createUserForm.classList.add('hidden');
            return loadUsers();
        }).catch(function(err) {
            setUsersMessage(err.message || '创建用户失败', true);
        }).finally(function() {
            dom.submitUserButton.disabled = false;
        });
    });

    dom.usersBody.addEventListener('click', function(e) {
        var button = e.target.closest('button[data-user-action]');
        if (!button) {
            var expandButton = e.target.closest('button[data-user-expand]');
            if (!expandButton) return;
            userExpansionState.toggle(expandButton.getAttribute('data-user-expand') || '');
            renderUsers();
            return;
        }
        var action = button.getAttribute('data-user-action');
        var username = button.getAttribute('data-username') || '';
        var request;
        if (action === 'toggle') {
            var active = button.getAttribute('data-active') === '1';
            if (!window.confirm((active ? '启用' : '禁用') + '用户“' + username + '”？禁用会立即注销其全部会话。')) return;
            request = send('ledger.user.set_active', { username: username, active: active ? 1 : 0 });
        } else if (action === 'password') {
            var password = window.prompt('请输入用户“' + username + '”的新密码：');
            if (password == null || password === '') return;
            request = send('ledger.user.update_pwd', { username: username, password: password });
        } else if (action === 'clear-password') {
            if (!window.confirm('清空用户“' + username + '”的密码？该用户全部会话将失效，下次登录时需要初始化密码。')) return;
            request = send('ledger.user.update_pwd', { username: username, password: '' });
        } else if (action === 'delete') {
            if (!window.confirm('软删除用户“' + username + '”？账号将停用，历史业务数据会保留。')) return;
            request = send('ledger.user.delete', { username: username, mode: 'soft' });
        }
        if (!request) return;
        button.disabled = true;
        request.then(loadUsers).catch(function(err) {
            setUsersMessage(err.message || '用户操作失败', true);
        }).finally(function() {
            button.disabled = false;
        });
    });

    if (dom.createIndependentInviteButton) {
        dom.createIndependentInviteButton.addEventListener('click', createIndependentInvite);
    }
    if (dom.copyIndependentInviteButton) {
        dom.copyIndependentInviteButton.addEventListener('click', copyIndependentInvite);
    }

    dom.currentUser.addEventListener('click', function() {
        window.location.reload();
    });

    dom.logoutButton.addEventListener('click', function() {
        logout();
    });

    window.addEventListener('beforeunload', function() {
        if (donationImageUrl) {
            URL.revokeObjectURL(donationImageUrl);
            donationImageUrl = '';
        }
    });

    dom.settingsPagination.addEventListener('click', function(e) {
        var button = e.target.closest('button[data-action="switch-group"]');
        if (!button) return;
        switchSettingsGroup(button.getAttribute('data-group-key') || '');
    });

    dom.settingsBody.addEventListener('click', function(e) {
        var button = e.target.closest('button[data-action]');
        if (!button) return;
        var action = button.getAttribute('data-action');
        var key = button.getAttribute('data-key') || '';

        if (action === 'save') {
            saveSetting(key);
        }
    });

    dom.settingsBody.addEventListener('keydown', function(e) {
        if (e.key !== 'Enter') return;
        var target = e.target;
        if (!target || target.tagName !== 'INPUT') return;
        var row = target.closest('li[data-key]');
        if (!row) return;
        e.preventDefault();
        saveSetting(row.getAttribute('data-key') || '');
    });

    ensureAdminSession().then(function() {
        loadSettings();
    });
})();
