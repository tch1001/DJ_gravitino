// One preparation path for live playback and editor preview. Transport-only
// Tutor setup intentionally retains physical controls and active live loops.
#include "TransitionPlayback.h"
#include "PlayerMath.h"

namespace gvt {

void prepareTransitionSetup(ControlBus& bus, AudioEngine& engine,
                            const GvtFile& file, int outgoingDeck,
                            TransitionSetupOptions options)
{
    const auto applyDeck = [&](bool fromRole, bool prepareTransport) {
        const int deckIndex = fromRole ? outgoingDeck : 1 - outgoingDeck;
        Deck& deck = engine.deck(deckIndex);
        const GvtInitialState& setup = fromRole ? file.initialFrom
                                                : file.initialTo;
        if (!setup.captured) return;
        if (prepareTransport)
            bus.dispatch({deckIndex, ControlId::Stop, 1.0}, Origin::System);
        if (options.applyHardwareFacingState && (!fromRole || options.applyOutgoingTempo))
            bus.dispatch({deckIndex, ControlId::Tempo,
                            transitionReplayTempoRatio(setup, fromRole ? file.from : file.to,
                                deck.track(), fromRole ? file.masterBpm : 0.0)},
                           Origin::System);
        const std::pair<ControlId, double> values[] = {
            {ControlId::Fader, setup.fader},
            {ControlId::EqLow, setup.eqLow},
            {ControlId::EqMid, setup.eqMid},
            {ControlId::EqHigh, setup.eqHigh},
            {ControlId::Filter, setup.filter},
        };
        if (options.applyHardwareFacingState)
            for (const auto& [control, value] : values)
                bus.dispatch({deckIndex, control, value}, Origin::System);
        if (!file.initialComplete) return;
        const std::pair<ControlId, double> extended[] = {
            {ControlId::FxType, (double)setup.fxType},
            {ControlId::FxOn, setup.fxOn ? 1.0 : 0.0},
            {ControlId::FxWet, setup.fxWet},
            {ControlId::FxBeats, setup.fxBeats},
            {ControlId::StemVocals, setup.stemVocals},
            {ControlId::StemMelody, setup.stemMelody},
            {ControlId::StemBass, setup.stemBass},
            {ControlId::StemDrums, setup.stemDrums},
        };
        if (options.applyHardwareFacingState)
            for (const auto& [control, value] : extended)
                bus.dispatch({deckIndex, control, value}, Origin::System);
        if (options.applyHardwareFacingState && setup.quantizeCaptured)
            bus.dispatch({deckIndex, ControlId::Quantize,
                            setup.quantize ? 1.0 : 0.0}, Origin::System);

        if (TrackDataPtr track = deck.track()) {
            // Tutor preparation must not change the audible/physical loop
            // state indirectly. In particular, replacing an active loop's
            // bounds and then seeking outside them makes Deck::seekSec exit
            // that loop. Preserve the complete live loop when hardware-facing
            // state is advisory; once the user exits it and retries PRIME, the
            // authored inactive bounds can be prepared safely.
            const bool preserveLiveLoop = !options.applyHardwareFacingState;
            const bool liveLoopActive = deck.loopActive.load();
            const double liveLoopStart = deck.loopStartSec.load();
            const double liveLoopEnd = deck.loopEndSec.load();
            deck.cuePointSec.store(transitionSecAtBeat(
                file, *track, setup.cueBeat));
            if (!preserveLiveLoop || !liveLoopActive) {
                deck.loopStartSec.store(transitionSecAtBeat(
                    file, *track, setup.loopStartBeat));
                deck.loopEndSec.store(transitionSecAtBeat(
                    file, *track, setup.loopEndBeat));
            }
            if (options.applyHardwareFacingState)
                deck.loopActive.store(setup.loopActive &&
                                      setup.loopEndBeat > setup.loopStartBeat);
            if (prepareTransport)
                deck.seekSec(transitionSecAtBeat(
                    file, *track, setup.positionBeat));
            if (preserveLiveLoop && liveLoopActive) {
                deck.loopStartSec.store(liveLoopStart);
                deck.loopEndSec.store(liveLoopEnd);
                deck.loopActive.store(true);
            }
        }
    };

    applyDeck(true, options.prepareOutgoingTransport);
    if (file.initialComplete)
        applyDeck(false, options.prepareIncomingTransport);
    else if (options.prepareIncomingTransport) {
        const int incoming = 1 - outgoingDeck;
        bus.dispatch({incoming, ControlId::Stop, 1.0}, Origin::System);
        if (TrackDataPtr track = engine.deck(incoming).track())
            engine.deck(incoming).seekSec(transitionSecAtBeat(
                file, *track, file.anchorToBeat));
    }
    if (options.applyHardwareFacingState && options.applyOutgoingTempo &&
        !file.initialFrom.captured) {
        const int outgoing = outgoingDeck;
        if (TrackDataPtr track = engine.deck(outgoing).track();
            track && track->bpm > 0.0 && file.masterBpm > 0.0)
            bus.dispatch({outgoing, ControlId::Tempo,
                            file.masterBpm / track->bpm},
                           Origin::System);
    }
}

void positionTransitionPerform(AudioEngine& engine, const GvtFile& file,
                               int outgoingDeck)
{
    if (const auto track = engine.deck(outgoingDeck).track())
        engine.deck(outgoingDeck).seekSec(transitionSecAtBeat(
            file, *track, file.anchorFromBeat));
}

double transitionEngineValue(AudioEngine& engine, int outgoingDeck,
                             Role role, ControlId control)
{
    if (role == Role::Mixer) return 0.0;
    const Deck& deck = engine.deck(role == Role::FromDeck
                                      ? outgoingDeck : 1 - outgoingDeck);
    switch (control) {
    case ControlId::Tempo: return deck.tempoRatio.load();
    case ControlId::Fader: return deck.fader.load();
    case ControlId::EqLow: return deck.eqLow.load();
    case ControlId::EqMid: return deck.eqMid.load();
    case ControlId::EqHigh: return deck.eqHigh.load();
    case ControlId::Filter: return deck.filter.load();
    case ControlId::FxWet: return deck.fxWet.load();
    case ControlId::FxBeats: return deck.fxBeats.load();
    case ControlId::FxType: return deck.fxType.load();
    case ControlId::FxOn: return deck.fxOn.load() ? 1.0 : 0.0;
    case ControlId::Quantize: return deck.quantizeHotCues.load() ? 1.0 : 0.0;
    case ControlId::StemVocals: return deck.stemVocals.load();
    case ControlId::StemMelody: return deck.stemMelody.load();
    case ControlId::StemBass: return deck.stemBass.load();
    case ControlId::StemDrums: return deck.stemDrums.load();
    default: return 0.0;
    }
}

void copyTransitionPlaybackContext(AudioEngine& source, int sourceOutgoing,
                                   ControlBus& targetBus, AudioEngine& target)
{
    const double xf = source.crossfader.load();
    targetBus.dispatch({kNoDeck, ControlId::Crossfader,
                        sourceOutgoing == 0 ? xf : 1.0 - xf}, Origin::System);
    for (Role role : {Role::FromDeck, Role::ToDeck}) {
        const int destination = role == Role::FromDeck ? 0 : 1;
        Deck& from = source.deck(destination == 0 ? sourceOutgoing
                                                  : 1 - sourceOutgoing);
        Deck& to = target.deck(destination);
        for (ControlId control : {ControlId::Tempo, ControlId::Fader,
                ControlId::EqLow, ControlId::EqMid, ControlId::EqHigh,
                ControlId::Filter, ControlId::FxType, ControlId::FxOn,
                ControlId::FxWet, ControlId::FxBeats, ControlId::Quantize,
                ControlId::StemVocals, ControlId::StemMelody,
                ControlId::StemBass, ControlId::StemDrums})
            targetBus.dispatch({destination, control,
                transitionEngineValue(source, sourceOutgoing, role, control)},
                Origin::System);
        targetBus.dispatch({destination, ControlId::Trim, from.trim.load()},
                           Origin::System);
        to.preservePitch.store(from.preservePitch.load());
        to.cuePointSec.store(from.cuePointSec.load());
        to.loopStartSec.store(from.loopStartSec.load());
        to.loopEndSec.store(from.loopEndSec.load());
        to.loopActive.store(from.loopActive.load());
    }
}

} // namespace gvt
