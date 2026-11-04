#include "DiskActions.h"

#include <filesystem>
#include <string>
#include <utility>

namespace {

// DiskSession speaks std::filesystem::path and UTF-8; JUCE speaks File and String.
std::filesystem::path toPath(const juce::File& file) {
  return std::filesystem::path(
      std::u8string(reinterpret_cast<const char8_t*>(file.getFullPathName().toRawUTF8())));
}

juce::String fromUtf8(const std::string& s) { return juce::String::fromUTF8(s.data(), (int)s.size()); }

juce::String text(const std::filesystem::path& p) {
  const auto u = p.u8string();
  return juce::String::fromUTF8(reinterpret_cast<const char*>(u.data()), (int)u.size());
}

juce::String fileName(const std::filesystem::path& p) { return text(p.filename()); }

}  // namespace

DiskActions::DiskActions(juce::Component& owner, mirage::DiskSession& session,
                         std::function<juce::File()> diskFolder,
                         std::function<void(const juce::File&)> setDiskFolder, std::function<void()> changed,
                         Guard guard)
    : owner_(owner),
      session_(session),
      diskFolder_(std::move(diskFolder)),
      setDiskFolder_(std::move(setDiskFolder)),
      changed_(std::move(changed)),
      guard_(std::move(guard)) {}

DiskActions::~DiskActions() = default;

void DiskActions::run(std::function<void()> work) {
  if (!guard_) return work();
  guard_([safe = juce::WeakReference<DiskActions>(this), work = std::move(work)] {
    if (safe != nullptr) work();
  });
}

bool DiskActions::dialogOpen() const {
  // Modal components include the alert and dialog windows and JUCE's
  // stand-in for a native file chooser.
  return promptOpen_ || juce::ModalComponentManager::getInstance()->getNumModalComponents() > 0;
}

void DiskActions::open() {
  run([this] {
    if (session_.saving() || promptOpen_) return;
    // Any file of exactly 450,560 bytes is a disk image, whatever its extension,
    // and so is a Giebler .edm file (detected by content).
    chooser_ = std::make_unique<juce::FileChooser>("Open Disk Image", diskFolder_(), "*.img;*.edm;*");
    juce::WeakReference<DiskActions> safe(this);
    chooser_->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe](const juce::FileChooser& fc) {
          const auto file = fc.getResult();
          if (safe == nullptr || file == juce::File()) return;
          safe->setDiskFolder_(file.getParentDirectory());
          safe->whenSafeToReplace("opening another disk", [w = safe.get(), file] { w->insert(file); });
        });
  });
}

void DiskActions::insert(const juce::File& file) {
  const auto error = session_.open(toPath(file));
  if (!error.empty()) showError("Cannot open disk image", fromUtf8(error));
  changed_();
}

void DiskActions::newBlank() {
  run([this] {
    if (session_.saving() || promptOpen_) return;
    whenSafeToReplace("inserting a new blank disk", [this] {
      if (!session_.newBlank()) showError("Cannot insert a blank disk", "The emulator is busy. Try again.");
      changed_();
    });
  });
}

void DiskActions::save() {
  run([this] { save(std::function<void()>()); });
}

void DiskActions::saveAs() {
  run([this] { saveAs(std::function<void()>()); });
}

void DiskActions::save(std::function<void()> then) {
  if (session_.path().empty()) return saveAs(std::move(then));  // untitled: ask for a file
  startSave({}, std::move(then));
}

void DiskActions::saveAs(std::function<void()> then) {
  if (session_.saving() || !session_.hasDisk()) return;
  // The dialog's default extension is taken from the suggested name: .edm only
  // when the session's format is Giebler .edm, else .img (a raw .edm would be
  // converted by accepting its own name). A .edm name saves Giebler .edm,
  // anything else a raw image (DiskSession::startSave).
  const auto& path = session_.path();
  const auto suggested =
      diskFolder_().getChildFile(path.empty() ? "Untitled" : text(path.stem()))
          .withFileExtension(session_.format() == mirage::DiskFormat::GieblerEdm ? ".edm" : ".img");
  chooser_ = std::make_unique<juce::FileChooser>("Save Disk Image As", suggested, "*.img;*.edm");
  juce::WeakReference<DiskActions> safe(this);
  chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                            juce::FileBrowserComponent::warnAboutOverwriting,
                        [safe, then = std::move(then)](const juce::FileChooser& fc) {
                          const auto file = fc.getResult();
                          if (safe == nullptr || file == juce::File()) return;
                          safe->setDiskFolder_(file.getParentDirectory());
                          safe->run([w = safe.get(), file, then] { w->startSave(file, then); });
                        });
}

void DiskActions::startSave(const juce::File& file, std::function<void()> then) {
  const auto target = file == juce::File() ? std::filesystem::path() : toPath(file);
  if (!session_.startSave(target)) {
    showError("Cannot save disk image", "The disk could not be saved right now. Try again.");
    return;
  }
  afterSave_ = std::move(then);
  changed_();
}

void DiskActions::eject() {
  whenSafeToReplace("ejecting it", [this] {
    session_.eject();
    changed_();
  });
}

void DiskActions::whenSafeToReplace(const juce::String& verb, std::function<void()> then) {
  run([this, verb, then = std::move(then)] {
    if (promptOpen_) return;
    if (session_.saving()) {
      // Ask again once the save in flight succeeds (the disk may be clean by then).
      if (!afterSave_) afterSave_ = [this, verb, then] { whenSafeToReplace(verb, then); };
      return;
    }
    if (!session_.needsSaveBeforeReplacing()) return then();
    promptOpen_ = true;
    juce::WeakReference<DiskActions> safe(this);
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions::makeOptionsYesNoCancel(
            juce::MessageBoxIconType::QuestionIcon, "Unsaved changes",
            "Save changes to " +
                (session_.path().empty() ? juce::String("the new disk") : fileName(session_.path())) +
                " before " + verb + "?",
            "Save", "Don't Save", "Cancel", &owner_),
        [safe, then](int choice) {  // 1 Save, 2 Don't Save, 0 Cancel
          if (safe == nullptr) return;
          safe->promptOpen_ = false;
          safe->run([w = safe.get(), choice, then] {
            if (choice == 1)
              w->save(then);
            else if (choice == 2)
              then();
          });
        });
  });
}

void DiskActions::showSaveOutcome(const mirage::SaveOutcome& outcome) {
  run([this, ok = outcome.ok, message = outcome.message] {
    auto then = std::exchange(afterSave_, nullptr);
    if (ok) {
      if (then) then();
    } else {
      showError("Cannot save disk image", fromUtf8(message));
    }
  });
}

void DiskActions::showError(const juce::String& title, const juce::String& message) {
  juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, title, message, {}, &owner_);
}
