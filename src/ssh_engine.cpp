#include "ssh_engine.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <libssh/libssh.h>

#include <algorithm>
#include <cstring>

#include "config.h"
#include "storage.h"

namespace {

// Modern algorithms supported by LibSSH-ESP32 (libssh 0.11.5 / mbedTLS)
constexpr const char* kModernKex =
    "curve25519-sha256,curve25519-sha256@libssh.org,"
    "ecdh-sha2-nistp256,ecdh-sha2-nistp384,ecdh-sha2-nistp521,"
    "diffie-hellman-group-exchange-sha256,diffie-hellman-group14-sha256,"
    "diffie-hellman-group16-sha512,diffie-hellman-group18-sha512";

constexpr const char* kModernHostKeys =
    "ssh-ed25519,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,"
    "ecdsa-sha2-nistp521,rsa-sha2-512,rsa-sha2-256";

constexpr const char* kModernCiphers =
    "chacha20-poly1305@openssh.com,aes256-gcm@openssh.com,"
    "aes128-gcm@openssh.com,aes256-ctr,aes128-ctr";

constexpr const char* kModernHmacs =
    "hmac-sha2-256-etm@openssh.com,hmac-sha2-512-etm@openssh.com,"
    "hmac-sha2-256,hmac-sha2-512";

// Legacy mode appends ssh-rsa, DH group1/14 sha1, AES-CBC, HMAC-SHA1
constexpr const char* kLegacyKex =
    "curve25519-sha256,curve25519-sha256@libssh.org,"
    "ecdh-sha2-nistp256,ecdh-sha2-nistp384,ecdh-sha2-nistp521,"
    "diffie-hellman-group-exchange-sha256,diffie-hellman-group14-sha256,"
    "diffie-hellman-group14-sha1,diffie-hellman-group1-sha1,"
    "diffie-hellman-group-exchange-sha1";

constexpr const char* kLegacyHostKeys =
    "ssh-ed25519,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,"
    "ecdsa-sha2-nistp521,rsa-sha2-512,rsa-sha2-256,ssh-rsa";

constexpr const char* kLegacyCiphers =
    "chacha20-poly1305@openssh.com,aes256-gcm@openssh.com,"
    "aes128-gcm@openssh.com,aes256-ctr,aes128-ctr,"
    "aes256-cbc,aes192-cbc,aes128-cbc";

constexpr const char* kLegacyHmacs =
    "hmac-sha2-256-etm@openssh.com,hmac-sha2-512-etm@openssh.com,"
    "hmac-sha2-256,hmac-sha2-512,hmac-sha1-etm@openssh.com,hmac-sha1";

constexpr uint32_t kSshTaskStackBytes = 32768;

struct KeyGenTaskCtx {
    SshKeyKind kind;
    std::string name;
    KeyGenResult result;
    SemaphoreHandle_t done = nullptr;
};

void keyGenTaskFn(void* arg) {
    auto* ctx = static_cast<KeyGenTaskCtx*>(arg);
    enum ssh_keytypes_e ktype = SSH_KEYTYPE_ED25519;
    int param = 0;
    if (ctx->kind == SshKeyKind::ECDSA_P256) {
        ktype = SSH_KEYTYPE_ECDSA_P256;
        param = 256;
    } else if (ctx->kind == SshKeyKind::RSA_2048) {
        ktype = SSH_KEYTYPE_RSA;
        param = 2048;
    }

    ssh_key key = nullptr;
    int rc = ssh_pki_generate(ktype, param, &key);
    if (rc != SSH_OK || key == nullptr) {
        ctx->result.ok = false;
        ctx->result.error = "ssh_pki_generate failed";
        xSemaphoreGive(ctx->done);
        vTaskDelete(nullptr);
        return;
    }

    char* priv_b64 = nullptr;
    rc = ssh_pki_export_privkey_base64_format(
        key, nullptr, nullptr, nullptr, &priv_b64, SSH_FILE_FORMAT_OPENSSH);
    if (rc != SSH_OK || priv_b64 == nullptr) {
        rc = ssh_pki_export_privkey_base64(key, nullptr, nullptr, nullptr, &priv_b64);
    }

    char* pub_b64 = nullptr;
    int pub_rc = ssh_pki_export_pubkey_base64(key, &pub_b64);
    const char* type_str = ssh_key_type_to_char(ssh_key_type(key));
    if (!type_str) type_str = "ssh-key";

    if (rc != SSH_OK || priv_b64 == nullptr || pub_rc != SSH_OK || pub_b64 == nullptr) {
        if (priv_b64) ssh_string_free_char(priv_b64);
        if (pub_b64) ssh_string_free_char(pub_b64);
        ssh_key_free(key);
        ctx->result.ok = false;
        ctx->result.error = "Key export failed";
        xSemaphoreGive(ctx->done);
        vTaskDelete(nullptr);
        return;
    }

    std::string priv_str(priv_b64);
    std::string pub_line = std::string(type_str) + " " + pub_b64 + " cardputer-adv\n";

    ssh_string_free_char(priv_b64);
    ssh_string_free_char(pub_b64);
    ssh_key_free(key);

    if (!storage::writeKeyFile(ctx->name, priv_str) ||
        !storage::writeKeyFile(ctx->name + ".pub", pub_line)) {
        ctx->result.ok = false;
        ctx->result.error = "Flash write failed";
    } else {
        ctx->result.ok = true;
        ctx->result.priv_name = ctx->name;
        ctx->result.pub_line = pub_line;
    }

    xSemaphoreGive(ctx->done);
    vTaskDelete(nullptr);
}

}  // namespace

SshEngine::SshEngine() {
    mutex_ = xSemaphoreCreateMutex();
}

SshEngine::~SshEngine() {
    disconnect();
    if (mutex_) {
        vSemaphoreDelete(static_cast<SemaphoreHandle_t>(mutex_));
        mutex_ = nullptr;
    }
}

void SshEngine::setState(State st, const std::string& status, const std::string& err) {
    auto mtx = static_cast<SemaphoreHandle_t>(mutex_);
    xSemaphoreTake(mtx, portMAX_DELAY);
    state_ = st;
    status_text_ = status;
    if (!err.empty()) {
        error_text_ = err;
    }
    xSemaphoreGive(mtx);
}

SshEngine::State SshEngine::state() const {
    return state_;
}

std::string SshEngine::statusText() const {
    auto mtx = static_cast<SemaphoreHandle_t>(mutex_);
    xSemaphoreTake(mtx, portMAX_DELAY);
    std::string out = status_text_;
    xSemaphoreGive(mtx);
    return out;
}

std::string SshEngine::errorText() const {
    auto mtx = static_cast<SemaphoreHandle_t>(mutex_);
    xSemaphoreTake(mtx, portMAX_DELAY);
    std::string out = error_text_;
    xSemaphoreGive(mtx);
    return out;
}

SshEngine::HostPromptInfo SshEngine::hostPromptInfo() const {
    auto mtx = static_cast<SemaphoreHandle_t>(mutex_);
    xSemaphoreTake(mtx, portMAX_DELAY);
    HostPromptInfo out = host_prompt_;
    xSemaphoreGive(mtx);
    return out;
}

void SshEngine::answerHostPrompt(bool accept) {
    auto mtx = static_cast<SemaphoreHandle_t>(mutex_);
    xSemaphoreTake(mtx, portMAX_DELAY);
    host_prompt_accepted_ = accept;
    host_prompt_answered_ = true;
    xSemaphoreGive(mtx);
}

SshEngine::InputPromptInfo SshEngine::inputPromptInfo() const {
    auto mtx = static_cast<SemaphoreHandle_t>(mutex_);
    xSemaphoreTake(mtx, portMAX_DELAY);
    InputPromptInfo out = input_prompt_;
    xSemaphoreGive(mtx);
    return out;
}

void SshEngine::answerInputPrompt(bool submitted, const std::string& value) {
    auto mtx = static_cast<SemaphoreHandle_t>(mutex_);
    xSemaphoreTake(mtx, portMAX_DELAY);
    input_prompt_value_ = value;
    input_prompt_submitted_ = submitted;
    input_prompt_answered_ = true;
    xSemaphoreGive(mtx);
}

size_t SshEngine::ringPush(RingBuf& rb, const uint8_t* src, size_t len) {
    size_t space = rb.cap - rb.count;
    size_t n = std::min(len, space);
    for (size_t i = 0; i < n; ++i) {
        rb.data[rb.head] = src[i];
        rb.head = (rb.head + 1) % rb.cap;
    }
    rb.count += n;
    return n;
}

size_t SshEngine::ringPop(RingBuf& rb, uint8_t* dst, size_t max_len) {
    size_t n = std::min(max_len, rb.count);
    for (size_t i = 0; i < n; ++i) {
        dst[i] = rb.data[rb.tail];
        rb.tail = (rb.tail + 1) % rb.cap;
    }
    rb.count -= n;
    return n;
}

size_t SshEngine::writeTx(const uint8_t* data, size_t len) {
    auto mtx = static_cast<SemaphoreHandle_t>(mutex_);
    xSemaphoreTake(mtx, portMAX_DELAY);
    size_t written = ringPush(tx_, data, len);
    xSemaphoreGive(mtx);
    return written;
}

size_t SshEngine::readRx(uint8_t* out, size_t max_len) {
    auto mtx = static_cast<SemaphoreHandle_t>(mutex_);
    xSemaphoreTake(mtx, portMAX_DELAY);
    size_t n = ringPop(rx_, out, max_len);
    xSemaphoreGive(mtx);
    return n;
}

bool SshEngine::startConnect(const HostProfile& profile) {
    disconnect();

    auto mtx = static_cast<SemaphoreHandle_t>(mutex_);
    xSemaphoreTake(mtx, portMAX_DELAY);
    profile_ = profile;
    if (profile_.port == 0) profile_.port = 22;
    stop_requested_ = false;
    state_ = State::CONNECTING;
    status_text_ = "Connecting...";
    error_text_.clear();
    rx_.head = rx_.tail = rx_.count = 0;
    tx_.head = tx_.tail = tx_.count = 0;
    host_prompt_answered_ = false;
    input_prompt_answered_ = false;
    xSemaphoreGive(mtx);

    TaskHandle_t handle = nullptr;
    BaseType_t ok = xTaskCreatePinnedToCore(
        taskEntry, "ssh_session", kSshTaskStackBytes, this, 2, &handle, 1);
    if (ok != pdPASS) {
        setState(State::ERROR, "Task error", "Failed to start SSH task");
        return false;
    }
    task_handle_ = handle;
    return true;
}

void SshEngine::disconnect() {
    stop_requested_ = true;
    // Wait up to 3s for session task to exit cleanly
    for (int i = 0; i < 150 && task_handle_ != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (state_ == State::CONNECTED || state_ == State::CONNECTING) {
        setState(State::DISCONNECTED, "Disconnected");
    }
}

void SshEngine::taskEntry(void* arg) {
    auto* self = static_cast<SshEngine*>(arg);
    self->runSession();
    self->task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

bool SshEngine::waitHostDecision() {
    while (!stop_requested_) {
        if (host_prompt_answered_) {
            return host_prompt_accepted_;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    return false;
}

bool SshEngine::waitInputValue(const std::string& title, const std::string& sub,
                               bool masked, std::string& out_val) {
    auto mtx = static_cast<SemaphoreHandle_t>(mutex_);
    xSemaphoreTake(mtx, portMAX_DELAY);
    input_prompt_.title = title;
    input_prompt_.subtitle = sub;
    input_prompt_.masked = masked;
    input_prompt_answered_ = false;
    input_prompt_submitted_ = false;
    input_prompt_value_.clear();
    state_ = State::PROMPT_INPUT;
    status_text_ = title;
    xSemaphoreGive(mtx);

    while (!stop_requested_) {
        if (input_prompt_answered_) {
            xSemaphoreTake(mtx, portMAX_DELAY);
            bool ok = input_prompt_submitted_;
            out_val = input_prompt_value_;
            input_prompt_value_.clear();
            xSemaphoreGive(mtx);
            return ok;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    return false;
}

void SshEngine::runSession() {
    ssh_session session = ssh_new();
    if (!session) {
        setState(State::ERROR, "Alloc error", "ssh_new failed");
        return;
    }

    unsigned int port = profile_.port;
    long timeout_sec = 15;
    int strict_check = 0;
    ssh_options_set(session, SSH_OPTIONS_HOST, profile_.host.c_str());
    ssh_options_set(session, SSH_OPTIONS_PORT, &port);
    if (!profile_.user.empty()) {
        ssh_options_set(session, SSH_OPTIONS_USER, profile_.user.c_str());
    }
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout_sec);
    ssh_options_set(session, SSH_OPTIONS_STRICTHOSTKEYCHECK, &strict_check);
    ssh_options_set(session, SSH_OPTIONS_PROCESS_CONFIG, &strict_check);

    if (profile_.legacy) {
        ssh_options_set(session, SSH_OPTIONS_KEY_EXCHANGE, kLegacyKex);
        ssh_options_set(session, SSH_OPTIONS_HOSTKEYS, kLegacyHostKeys);
        ssh_options_set(session, SSH_OPTIONS_PUBLICKEY_ACCEPTED_TYPES, kLegacyHostKeys);
        ssh_options_set(session, SSH_OPTIONS_CIPHERS_C_S, kLegacyCiphers);
        ssh_options_set(session, SSH_OPTIONS_CIPHERS_S_C, kLegacyCiphers);
        ssh_options_set(session, SSH_OPTIONS_HMAC_C_S, kLegacyHmacs);
        ssh_options_set(session, SSH_OPTIONS_HMAC_S_C, kLegacyHmacs);
    } else {
        ssh_options_set(session, SSH_OPTIONS_KEY_EXCHANGE, kModernKex);
        ssh_options_set(session, SSH_OPTIONS_HOSTKEYS, kModernHostKeys);
        ssh_options_set(session, SSH_OPTIONS_PUBLICKEY_ACCEPTED_TYPES, kModernHostKeys);
        ssh_options_set(session, SSH_OPTIONS_CIPHERS_C_S, kModernCiphers);
        ssh_options_set(session, SSH_OPTIONS_CIPHERS_S_C, kModernCiphers);
        ssh_options_set(session, SSH_OPTIONS_HMAC_C_S, kModernHmacs);
        ssh_options_set(session, SSH_OPTIONS_HMAC_S_C, kModernHmacs);
    }

    setState(State::CONNECTING, "TCP + SSH handshake...");
    int rc = ssh_connect(session);
    if (rc != SSH_OK || stop_requested_) {
        std::string err = stop_requested_ ? "Cancelled" : ssh_get_error(session);
        ssh_free(session);
        setState(stop_requested_ ? State::DISCONNECTED : State::ERROR, "Connect failed", err);
        return;
    }

    // Host key verification (TOFU backed by LittleFS /known_hosts.csv)
    ssh_key srv_pubkey = nullptr;
    if (ssh_get_server_publickey(session, &srv_pubkey) != SSH_OK || !srv_pubkey) {
        ssh_disconnect(session);
        ssh_free(session);
        setState(State::ERROR, "Host key error", "Failed to read server key");
        return;
    }
    unsigned char* hash = nullptr;
    size_t hlen = 0;
    std::string fp_str = "unknown";
    if (ssh_get_publickey_hash(srv_pubkey, SSH_PUBLICKEY_HASH_SHA256, &hash, &hlen) == SSH_OK) {
        char* fp = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hlen);
        if (fp) {
            fp_str = fp;
            ssh_string_free_char(fp);
        }
        ssh_clean_pubkey_hash(&hash);
    }
    const char* kt = ssh_key_type_to_char(ssh_key_type(srv_pubkey));
    std::string key_type = kt ? kt : "unknown";
    ssh_key_free(srv_pubkey);

    std::string saved_fp = storage::findKnownHostFingerprint(profile_.host, profile_.port);
    if (saved_fp != fp_str) {
        auto mtx = static_cast<SemaphoreHandle_t>(mutex_);
        xSemaphoreTake(mtx, portMAX_DELAY);
        host_prompt_.host = profile_.host;
        host_prompt_.port = profile_.port;
        host_prompt_.key_type = key_type;
        host_prompt_.fingerprint = fp_str;
        host_prompt_.changed = !saved_fp.empty();
        host_prompt_answered_ = false;
        host_prompt_accepted_ = false;
        state_ = State::PROMPT_HOST_KEY;
        status_text_ = host_prompt_.changed ? "HOST KEY CHANGED!" : "Verify host key";
        xSemaphoreGive(mtx);

        if (!waitHostDecision()) {
            ssh_disconnect(session);
            ssh_free(session);
            setState(State::DISCONNECTED, "Host key rejected");
            return;
        }
        storage::upsertKnownHost(profile_.host, profile_.port, fp_str);
    }

    // Authentication
    setState(State::CONNECTING, "Authenticating...");
    int auth_rc = ssh_userauth_none(session, nullptr);
    int methods = ssh_userauth_list(session, nullptr);

    // 1. Try public key auth if configured
    if (auth_rc != SSH_AUTH_SUCCESS && !profile_.key_file.empty() && !stop_requested_) {
        std::string key_b64;
        if (!storage::readKeyFile(profile_.key_file, key_b64)) {
            ssh_disconnect(session);
            ssh_free(session);
            setState(State::ERROR, "Key missing", "Cannot read /keys/" + profile_.key_file);
            return;
        }

        ssh_key privkey = nullptr;
        int krc = ssh_pki_import_privkey_base64(
            key_b64.c_str(), nullptr, nullptr, nullptr, &privkey);
        if (krc != SSH_OK) {
            std::string passphrase;
            if (waitInputValue("Key Passphrase", profile_.key_file, true, passphrase)) {
                krc = ssh_pki_import_privkey_base64(
                    key_b64.c_str(), passphrase.c_str(), nullptr, nullptr, &privkey);
            }
        }

        if (krc == SSH_OK && privkey != nullptr) {
            setState(State::CONNECTING, "Auth with key...");
            auth_rc = ssh_userauth_publickey(session, nullptr, privkey);
            ssh_key_free(privkey);
        }
    }

    // 2. Password / keyboard-interactive auth fallback
    if (auth_rc != SSH_AUTH_SUCCESS && !stop_requested_) {
        std::string pw = profile_.password;
        if (pw.empty()) {
            std::string sub = profile_.user + "@" + profile_.host;
            if (!waitInputValue("SSH Password", sub, true, pw)) {
                ssh_disconnect(session);
                ssh_free(session);
                setState(State::DISCONNECTED, "Auth cancelled");
                return;
            }
        }

        setState(State::CONNECTING, "Auth with password...");
        if ((methods == 0) || (methods & SSH_AUTH_METHOD_PASSWORD)) {
            auth_rc = ssh_userauth_password(session, nullptr, pw.c_str());
        }
        if (auth_rc != SSH_AUTH_SUCCESS && (methods & SSH_AUTH_METHOD_INTERACTIVE) && !stop_requested_) {
            int kbd = ssh_userauth_kbdint(session, nullptr, nullptr);
            while (kbd == SSH_AUTH_INFO && !stop_requested_) {
                int nprompts = ssh_userauth_kbdint_getnprompts(session);
                for (int i = 0; i < nprompts; ++i) {
                    char echo = 0;
                    const char* prompt = ssh_userauth_kbdint_getprompt(session, i, &echo);
                    std::string ans;
                    if (nprompts == 1 && !pw.empty()) {
                        ans = pw;
                    } else {
                        std::string ptxt = prompt ? prompt : "Response:";
                        if (!waitInputValue("Auth Prompt", ptxt, echo == 0, ans)) {
                            kbd = SSH_AUTH_DENIED;
                            break;
                        }
                    }
                    ssh_userauth_kbdint_setanswer(session, i, ans.c_str());
                }
                if (kbd == SSH_AUTH_DENIED) break;
                kbd = ssh_userauth_kbdint(session, nullptr, nullptr);
            }
            auth_rc = kbd;
        }
    }

    if (auth_rc != SSH_AUTH_SUCCESS || stop_requested_) {
        std::string err = stop_requested_ ? "Cancelled" : ssh_get_error(session);
        if (err.empty()) err = "Authentication failed";
        ssh_disconnect(session);
        ssh_free(session);
        setState(stop_requested_ ? State::DISCONNECTED : State::ERROR, "Auth failed", err);
        return;
    }

    // Open PTY + Shell
    ssh_channel channel = ssh_channel_new(session);
    if (!channel || ssh_channel_open_session(channel) != SSH_OK) {
        std::string err = ssh_get_error(session);
        if (channel) ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);
        setState(State::ERROR, "Channel error", err);
        return;
    }

    if (ssh_channel_request_pty_size(channel, "xterm", cfg::TERM_COLS, cfg::TERM_ROWS) != SSH_OK ||
        ssh_channel_request_shell(channel) != SSH_OK) {
        std::string err = ssh_get_error(session);
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);
        setState(State::ERROR, "Shell error", err);
        return;
    }

    setState(State::CONNECTED, "Connected");

    uint8_t io_buf[512];
    while (!stop_requested_ && ssh_channel_is_open(channel) && !ssh_channel_is_eof(channel)) {
        bool active = false;

        // Flush outbound keystrokes
        size_t tx_len = 0;
        {
            auto mtx = static_cast<SemaphoreHandle_t>(mutex_);
            xSemaphoreTake(mtx, portMAX_DELAY);
            tx_len = ringPop(tx_, io_buf, sizeof(io_buf));
            xSemaphoreGive(mtx);
        }
        if (tx_len > 0) {
            int wn = ssh_channel_write(channel, io_buf, static_cast<uint32_t>(tx_len));
            if (wn < 0) break;
            active = true;
        }

        // Read stdout & stderr non-blocking
        for (int is_stderr = 0; is_stderr <= 1; ++is_stderr) {
            int rn = ssh_channel_read_nonblocking(channel, io_buf, sizeof(io_buf), is_stderr);
            if (rn > 0) {
                auto mtx = static_cast<SemaphoreHandle_t>(mutex_);
                xSemaphoreTake(mtx, portMAX_DELAY);
                ringPush(rx_, io_buf, static_cast<size_t>(rn));
                xSemaphoreGive(mtx);
                active = true;
            } else if (rn == SSH_ERROR) {
                stop_requested_ = true;
                break;
            }
        }

        if (!active) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    ssh_disconnect(session);
    ssh_free(session);

    setState(State::DISCONNECTED, "Session closed");
}

KeyGenResult SshEngine::generateKeyPair(SshKeyKind kind, const std::string& name) {
    KeyGenTaskCtx ctx;
    ctx.kind = kind;
    ctx.name = name;
    ctx.done = xSemaphoreCreateBinary();
    if (!ctx.done) {
        KeyGenResult r;
        r.error = "Semaphore failed";
        return r;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        keyGenTaskFn, "ssh_keygen", kSshTaskStackBytes, &ctx, 2, nullptr, 1);
    if (ok != pdPASS) {
        vSemaphoreDelete(ctx.done);
        KeyGenResult r;
        r.error = "Task alloc failed";
        return r;
    }

    xSemaphoreTake(ctx.done, portMAX_DELAY);
    vSemaphoreDelete(ctx.done);
    return ctx.result;
}
