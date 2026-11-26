#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "csv_codec.h"

enum class SshKeyKind {
    ED25519 = 0,
    ECDSA_P256 = 1,
    RSA_2048 = 2,
};

struct KeyGenResult {
    bool ok = false;
    std::string priv_name;
    std::string pub_line;
    std::string error;
};

class SshEngine {
public:
    enum class State {
        IDLE,
        CONNECTING,
        PROMPT_HOST_KEY,
        PROMPT_INPUT,
        CONNECTED,
        DISCONNECTED,
        ERROR,
    };

    SshEngine();
    ~SshEngine();

    bool startConnect(const HostProfile& profile);
    void disconnect();

    State state() const;
    std::string statusText() const;
    std::string errorText() const;

    // Host key verification prompt data & answer
    struct HostPromptInfo {
        std::string host;
        uint16_t port = 22;
        std::string key_type;
        std::string fingerprint;
        bool changed = false;
    };
    HostPromptInfo hostPromptInfo() const;
    void answerHostPrompt(bool accept);

    // Interactive password / passphrase prompt data & answer
    struct InputPromptInfo {
        std::string title;
        std::string subtitle;
        bool masked = true;
    };
    InputPromptInfo inputPromptInfo() const;
    void answerInputPrompt(bool submitted, const std::string& value);

    // Terminal stream I/O
    size_t writeTx(const uint8_t* data, size_t len);
    size_t readRx(uint8_t* out, size_t max_len);

    // On-device key generation in a 32KB stack FreeRTOS task
    static KeyGenResult generateKeyPair(SshKeyKind kind, const std::string& name);

private:
    static constexpr size_t kRxBufSize = 8192;
    static constexpr size_t kTxBufSize = 1024;

    struct RingBuf {
        uint8_t* data = nullptr;
        size_t cap = 0;
        size_t head = 0;
        size_t tail = 0;
        size_t count = 0;
    };

    HostProfile profile_;
    volatile State state_ = State::IDLE;
    volatile bool stop_requested_ = false;

    void* task_handle_ = nullptr;
    mutable void* mutex_ = nullptr;

    std::string status_text_;
    std::string error_text_;

    HostPromptInfo host_prompt_;
    volatile bool host_prompt_answered_ = false;
    volatile bool host_prompt_accepted_ = false;

    InputPromptInfo input_prompt_;
    volatile bool input_prompt_answered_ = false;
    volatile bool input_prompt_submitted_ = false;
    std::string input_prompt_value_;

    uint8_t rx_storage_[kRxBufSize]{};
    uint8_t tx_storage_[kTxBufSize]{};
    RingBuf rx_{rx_storage_, kRxBufSize, 0, 0, 0};
    RingBuf tx_{tx_storage_, kTxBufSize, 0, 0, 0};

    static void taskEntry(void* arg);
    void runSession();
    void setState(State st, const std::string& status, const std::string& err = "");
    bool waitHostDecision();
    bool waitInputValue(const std::string& title, const std::string& sub, bool masked, std::string& out_val);

    size_t ringPush(RingBuf& rb, const uint8_t* src, size_t len);
    size_t ringPop(RingBuf& rb, uint8_t* dst, size_t max_len);
};
