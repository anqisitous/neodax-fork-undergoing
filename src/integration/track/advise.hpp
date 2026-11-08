// advise.hpp — neodax の「生成的アプリ解析 / IT アドバイザ」パターン
//
// 目的:
//   状態の事実 (Signal) を集め、持続性 (Durable/Transient) で分類し、
//   クラス化した判定 (LoginModel) を通して Verdict と Advice を出す。
//   表示は HTML ではなく、DVNode と同じ発想のベクタ (SVG) に閉じる。
//
// クラス:
//   Signal       観測した事実。Durable か Transient かを持つ
//   LoginModel   AE の起動/ログイン判定。Durable だけで Verdict を決める
//   Advice       判定に応じた次の手順
//   AdvisorySvg  LoginModel を非 HTML の SVG ボードにする
//
// 設計上の約束:
//   Transient (NGL コンテナの現在の有無など) は判定に使わない。
//   予測は必ず observed_at 付きのスナップショットとして出す。

#ifndef NX_ADVISE_HPP
#define NX_ADVISE_HPP

#include <cstdio>
#include <string>
#include <vector>

namespace nx {

enum class Persistence { Durable, Transient };
enum class Verdict { No, Yes, Unknown };

struct Signal {
    std::string id;         // "OOBE.LoggedOutModeEnabled"
    std::string source;     // ファイル / プローブ
    std::string value;      // 観測値 ("true" / "SIGNIN" / "(無し)")
    Persistence persistence = Persistence::Durable;
    bool observed = false;
    bool informs_verdict = true;   // false なら表示だけ
};

struct Advice {
    std::string summary;
    std::vector<std::string> steps;
};

// 承認ダイアログを出さずに進められる操作と、OS 権限の対応。
// available=false は「その権限がないと実行できない」ことを示す (実行はしない)。
struct Capability {
    std::string action;
    std::string permission;      // file-read / file-write / AX / CGWindowList / keychain
    bool requires_approval = false;
    bool available = true;
};

// ------------------------------------------------------------------
// タブの IT アドバイザ
//   「削除していいか」を先に判定して、理由付きで提示する。
//   削除そのものは行わない (判断と実行を分ける)。
// ------------------------------------------------------------------

enum class TabKind { Chat, SurfaceLive, SurfaceOrphan, Unknown };

struct TabFact {
    std::string id;
    std::string surface;
    std::string owner;
    TabKind kind = TabKind::Unknown;
};

class TabModel {
public:
    explicit TabModel(std::string state_json, std::string source);
    void collect();
    Verdict evaluate() const;   // Yes = 閉じられないタブがある
    const std::vector<TabFact> &tabs() const { return tabs_; }
    const std::vector<Capability> &capabilities() const { return caps_; }
    const Advice &advice() const { return advice_; }
    std::string observed_at() const { return observed_at_; }
    std::string source() const { return source_; }

    // この id を「削除してよい」と分類上言えるか。
    bool removable(const std::string &id, std::string &why) const;

private:
    std::string json_;
    std::string source_;
    std::string observed_at_;
    std::vector<TabFact> tabs_;
    std::vector<Capability> caps_;
    Advice advice_;
    int orphans_ = 0;
    int live_surfaces_ = 0;
    void build_advice();
    void build_capabilities();
};

class LoginModel {
public:
    explicit LoginModel(std::string home);
    void collect();
    Verdict evaluate() const;
    const std::vector<Signal> &signals() const { return signals_; }
    const std::vector<Capability> &capabilities() const { return caps_; }
    const Advice &advice() const { return advice_; }
    std::string observed_at() const { return observed_at_; }

private:
    std::string home_;
    std::string observed_at_;
    std::vector<Signal> signals_;
    std::vector<Capability> caps_;
    Advice advice_;
    bool logged_out_ = false;
    bool logged_in_flag_ = false;
    bool sso_ = false;
    void build_advice();
    void build_capabilities();
};

class AdvisorySvg {
public:
    // 成功なら true。err に理由を入れる。
    static bool write(const LoginModel &m, const std::string &path, std::string &err);
};

} // namespace nx

#endif
