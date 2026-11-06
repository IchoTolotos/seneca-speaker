#include "test.h"
#include "voice_control.h"
#include "session_fsm.h"

static void fill(int16_t *pcm, size_t n, int16_t value)
{
    for (size_t i = 0; i < n; ++i) pcm[i] = i % 2 ? value : (int16_t)-value;
}

static void interruption_requires_sustained_speech(void)
{
    sc_barge_in_t s = {0};
    int16_t pcm[160];
    fill(pcm, 160, 2000);
    for (uint64_t t = 0; t < 120; t += 10)
        CHECK(!sc_barge_in_feed(&s, pcm, 160, t, true, false));
    CHECK(sc_barge_in_feed(&s, pcm, 160, 120, true, false));
    for (uint64_t t = 130; t < 920; t += 10)
        CHECK(!sc_barge_in_feed(&s, pcm, 160, t, true, false));
    CHECK(sc_barge_in_feed(&s, pcm, 160, 920, true, false));
}

static void noise_mute_and_inactive_audio_do_not_interrupt(void)
{
    sc_barge_in_t s = {0};
    int16_t pcm[160];
    fill(pcm, 160, 1000);
    for (uint64_t t = 0; t < 1000; t += 10)
        CHECK(!sc_barge_in_feed(&s, pcm, 160, t, true, false));
    fill(pcm, 160, 32767);
    CHECK(!sc_barge_in_feed(&s, pcm, 160, 1000, true, false));
    CHECK(!sc_barge_in_feed(&s, pcm, 160, 1110, true, false));
    CHECK(!sc_barge_in_feed(&s, pcm, 160, 1120, true, true));
    CHECK(!sc_barge_in_feed(&s, pcm, 160, 1130, true, false));
    CHECK(!sc_barge_in_feed(&s, pcm, 160, 1250, false, false));
    CHECK(!sc_barge_in_feed(&s, pcm, 160, 1260, true, false));
    CHECK(sc_barge_in_feed(&s, pcm, 160, 1380, true, false));
    CHECK(!sc_barge_in_feed(&s, NULL, 160, 3000, true, false));
}

static void hangup_requires_an_addressed_command(void)
{
    CHECK(sc_voice_is_hangup("Seneca, stop!"));
    // both spoken forms, any punctuation/case the ASR may produce
    CHECK(sc_voice_is_hangup("Seneca stop"));
    CHECK(sc_voice_is_hangup("Seneca, stop"));
    CHECK(sc_voice_is_hangup("seneca, stop."));
    CHECK(sc_voice_is_hangup("Seneca stop."));
    CHECK(sc_voice_is_hangup("Seneca. Stop."));
    CHECK(sc_voice_is_hangup("Seneca,stop"));
    CHECK(sc_voice_is_hangup("SENECA STOP!"));
    CHECK(sc_voice_is_hangup("Seneka stopp"));
    CHECK(sc_voice_is_hangup("Seneka stopp."));
    CHECK(!sc_voice_is_hangup("Seneca, please stop."));
    CHECK(!sc_voice_is_hangup("Please end the call."));
    CHECK(!sc_voice_is_hangup("End this call please"));
    CHECK(!sc_voice_is_hangup("Please hang up now."));
    CHECK(!sc_voice_is_hangup("Bitte beende den Anruf!"));
    CHECK(!sc_voice_is_hangup("Seneca, hang up!"));
    CHECK(!sc_voice_is_hangup(" SENECA please hang up. "));
    CHECK(!sc_voice_is_hangup("Seneca, hang up please"));
    CHECK(!sc_voice_is_hangup("Seneca, auflegen."));
    CHECK(!sc_voice_is_hangup("Seneka, bitte auflegen!"));
    CHECK(!sc_voice_is_hangup("Seneca, leg bitte auf."));
    CHECK(!sc_voice_is_hangup("auflegen"));
    CHECK(!sc_voice_is_hangup("bitte auflegen"));
    CHECK(!sc_voice_is_hangup("stop"));
    CHECK(!sc_voice_is_hangup("Please stop reading"));
    CHECK(!sc_voice_is_hangup("Do not end the call"));
    CHECK(!sc_voice_is_hangup("Why did you end the call"));
    CHECK(!sc_voice_is_hangup("Please end the call when I finish"));
    CHECK(!sc_voice_is_hangup("Seneca stop talking about that"));
    CHECK(!sc_voice_is_hangup("Do not hang up"));
    CHECK(!sc_voice_is_hangup("Seneca, don't hang up"));
    CHECK(!sc_voice_is_hangup("Tell me how to say Seneca hang up"));
    CHECK(!sc_voice_is_hangup("Seneca hang up later"));
    CHECK(!sc_voice_is_hangup("Seneca hang up 2"));
    CHECK(!sc_voice_is_hangup("Seneca stop speaking"));
    CHECK(!sc_voice_is_hangup("hang up"));
    CHECK(!sc_voice_is_hangup(NULL));
    char long_text[256];
    memset(long_text, 'a', sizeof long_text);long_text[sizeof long_text-1]=0;
    CHECK(!sc_voice_is_hangup(long_text));
}

static void stop_phrase_handles_fragments_and_waits_for_completion(void)
{
    sc_voice_stop_t s = {0};
    sc_voice_stop_update(&s,"Seneca",1000);
    sc_voice_stop_update(&s," stopp.",1200);
    CHECK(!sc_voice_stop_poll(&s,1699));
    CHECK(sc_voice_stop_poll(&s,1700));
    CHECK(!sc_voice_stop_poll(&s,1800));
    sc_voice_stop_update(&s,"stop",2000);
    CHECK(!sc_voice_stop_poll(&s,3000)); // never an unaddressed stop
    sc_voice_stop_update(&s,"Seneca",4000);
    sc_voice_stop_update(&s,"auflegen",4200);
    CHECK(!sc_voice_stop_poll(&s,5000));
    sc_voice_stop_update(&s,"Seneca",6000);
    sc_voice_stop_update(&s,"stop",9100);
    CHECK(!sc_voice_stop_poll(&s,10000)); // prefix expired
    sc_voice_stop_update(&s,"Seneca stop",11000);
    sc_voice_stop_update(&s,"Seneca stop talking about that",11200);
    CHECK(!sc_voice_stop_poll(&s,12000)); // interim command was part of a longer utterance
    sc_voice_stop_update(&s,"Seneca stop",13000);
    CHECK(sc_voice_stop_poll(&s,13500));
}

static void queued_interrupt_never_starts_a_call(void)
{
    sc_fsm_t f;sc_fsm_init(&f,NULL,true,0);
    sc_fsm_input_t in = {.type=SC_IN_BARGE_IN,.now_ms=1000};
    sc_fsm_output_t out;
    f.state=SC_ST_IDLE;
    sc_fsm_handle(&f,&in,&out);
    CHECK(out.actions==0);CHECK(f.state==SC_ST_IDLE);
    f.state=SC_ST_IN_CALL;f.remote_state=SC_RT_SPEAKING;
    sc_fsm_handle(&f,&in,&out);
    CHECK(out.actions==SC_ACT_STOP_SPEAKING);CHECK(f.state==SC_ST_IN_CALL);
    f.muted=true;
    sc_fsm_handle(&f,&in,&out);CHECK(out.actions==0);
    f.muted=false;f.remote_state=SC_RT_LISTENING;
    sc_fsm_handle(&f,&in,&out);CHECK(out.actions==0);
    in.type=SC_IN_HANGUP_REQUEST;
    sc_fsm_handle(&f,&in,&out);
    CHECK(out.actions&SC_ACT_HANG_UP);CHECK(f.state==SC_ST_IDLE);
    in.type=SC_IN_BARGE_IN;
    sc_fsm_handle(&f,&in,&out);CHECK(out.actions==0);
}

void suite_voice_control(void)
{
    RUN(interruption_requires_sustained_speech);
    RUN(noise_mute_and_inactive_audio_do_not_interrupt);
    RUN(hangup_requires_an_addressed_command);
    RUN(stop_phrase_handles_fragments_and_waits_for_completion);
    RUN(queued_interrupt_never_starts_a_call);
}
