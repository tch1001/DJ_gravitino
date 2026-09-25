// Ableton-style source slice and piano-roll workspace. All edits go through
// the transition document's undo stack; this view never writes audio or recipes.
#pragma once
#include "../analysis/TrackData.h"
#include "../transitions/TonePlay.h"
#include <QWidget>
class QCheckBox;
class QDoubleSpinBox;
class QComboBox;
class QLabel;
class QScrollBar;
class QPushButton;
namespace gvt {
class TransitionEditorDocument;
class ToneSampleView;
class TonePianoRoll;
class TonePlayEditor final : public QWidget {
    Q_OBJECT
  public:
    explicit TonePlayEditor(TransitionEditorDocument *, QWidget *parent = nullptr);
    void setTrack(TrackDataPtr);
    void setPlayhead(double);
    void zoom(double factor);
    void refresh();
  signals:
    void auditionRequested(int pitch);
    void sustainAuditionRequested();
    void previewRequested();
    void cursorRequested(double beat);

  private:
    bool commit(TonePlayPattern, const QString &);
    void refreshNote();
    TransitionEditorDocument *document_;
    TrackDataPtr track_;
    ToneSampleView *sample_;
    TonePianoRoll *roll_;
    QScrollBar *sampleScroll_;
    QCheckBox *enabled_;
    QCheckBox *replace_;
    QCheckBox *sustainEnabled_, *editSustain_;
    QPushButton *sustainPreview_;
    QDoubleSpinBox *holdBeat_, *holdDuration_, *holdGain_, *loopStart_, *loopEnd_;
    QDoubleSpinBox *crossfade_, *attack_, *release_;
    QComboBox *holdPitch_;
    QCheckBox *effectsEnabled_;
    QDoubleSpinBox *echo_, *reverb_, *echoBeats_, *tailBeats_, *holdDucking_;
    QDoubleSpinBox *start_, *end_, *gain_, *noteBeat_, *noteDuration_, *noteVelocity_;
    QComboBox *root_, *notePitch_, *snap_;
    QLabel *help_, *sliceInfo_;
    bool refreshing_ = false;
};
} // namespace gvt
