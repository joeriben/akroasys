// aqrec — exact capture from one CoreAudio device.
// tools/verify_lro_in_standalone.py builds and drives it.
//
//   aqrec --list                    one line per device:
//                                   UID <tab> input ch <tab> output ch <tab> name
//   aqrec UID SECONDS OUT.wav       record 48 kHz stereo int16; prints "recording NS"
//                                   once the first buffer is in, NS = the capture
//                                   time of the take's first sample on the
//                                   mach_absolute_time clock in nanoseconds (the
//                                   clock Python's time.monotonic_ns() reads on macOS)
//
// Why not ffmpeg: its avfoundation capture of BlackHole 2ch at 48 kHz drops frames
// on the maintainer Mac (ffmpeg 9.0.2, measured 2026-10-13 and again 2026-10-14):
// tone bursts played exactly 1.000 s apart come back 0.87-0.90 s apart with the
// pitch intact, and `-t 8` yields a 7.1 s file. Every take is spliced. AudioQueue on
// the same chain returns 1.000 s.
//
// A recorder that falls behind does not shorten a take here, it puts SILENCE into
// it: across a stall the queue's own sample times stay contiguous and the lost
// stretch comes back as zeros (measured). Two checks catch that, and either one
// still writes the file but exits 3:
//   - the device's processor-overload notification, which the HAL sends when this
//     process's IO thread missed a cycle (raised by a 0.05, 0.1 and 1 s SIGSTOP of
//     the recorder; never on a clean run, nor when another process stalls);
//   - lateness: how long after its data's capture time each buffer reaches the
//     callback. If only the callback thread stalls, the IO thread stays on time and
//     no overload fires; a 1.6 s stall there lost audio silently. Any buffer later
//     than MAX_LATE fails the take. The callback only copies into memory sized for
//     the whole take, so the file write cannot be what stalls it.
//
// Build: clang -O2 -o aqrec tools/aqrec.c -framework AudioToolbox \
//            -framework CoreAudio -framework CoreFoundation
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { RATE = 48000, CHANNELS = 2, BYTES_PER_FRAME = 4, BUFFER_FRAMES = 2048 };
static const double MAX_LATE = 0.25;     // s; see the lateness note above
static const long STALL_MS = 5000;       // no audio moving for this long is a hang

static AudioStreamBasicDescription pcm_format(void) {
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = RATE;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    f.mBytesPerPacket = BYTES_PER_FRAME;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = BYTES_PER_FRAME;
    f.mChannelsPerFrame = CHANNELS;
    f.mBitsPerChannel = 16;
    return f;
}

static int use_device(AudioQueueRef q, const char *uid_text) {
    CFStringRef uid = CFStringCreateWithCString(NULL, uid_text, kCFStringEncodingUTF8);
    OSStatus s = AudioQueueSetProperty(q, kAudioQueueProperty_CurrentDevice, &uid, sizeof uid);
    CFRelease(uid);
    if (s) fprintf(stderr, "no device with UID %s (%d)\n", uid_text, (int)s);
    return s ? 1 : 0;
}

// ---- --list --------------------------------------------------------------------

static void string_property(AudioObjectID id, AudioObjectPropertySelector sel,
                            char *buf, size_t len) {
    AudioObjectPropertyAddress a = {sel, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    CFStringRef s = NULL;
    UInt32 size = sizeof s;
    buf[0] = 0;
    if (AudioObjectGetPropertyData(id, &a, 0, NULL, &size, &s) == noErr && s) {
        CFStringGetCString(s, buf, (CFIndex)len, kCFStringEncodingUTF8);
        CFRelease(s);
    }
}

static unsigned channel_count(AudioObjectID id, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress a = {kAudioDevicePropertyStreamConfiguration, scope,
                                    kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(id, &a, 0, NULL, &size) != noErr || !size) return 0;
    AudioBufferList *list = malloc(size);
    unsigned n = 0;
    if (AudioObjectGetPropertyData(id, &a, 0, NULL, &size, list) == noErr)
        for (UInt32 i = 0; i < list->mNumberBuffers; i++) n += list->mBuffers[i].mNumberChannels;
    free(list);
    return n;
}

static int list_devices(void) {
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDevices,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size)) return 1;
    AudioObjectID *ids = malloc(size);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, ids)) {
        free(ids);
        return 1;
    }
    for (UInt32 i = 0; i < size / sizeof *ids; i++) {
        char uid[512], name[512];
        string_property(ids[i], kAudioDevicePropertyDeviceUID, uid, sizeof uid);
        string_property(ids[i], kAudioObjectPropertyName, name, sizeof name);
        printf("%s\t%u\t%u\t%s\n", uid, channel_count(ids[i], kAudioObjectPropertyScopeInput),
               channel_count(ids[i], kAudioObjectPropertyScopeOutput), name);
    }
    free(ids);
    return 0;
}

// ---- record ----------------------------------------------------------------------

static unsigned char *take;              // the whole take, allocated before it starts
static long frames_written, frames_target;
static volatile int overloads;
static int started, done;                // __atomic: first_host, then started, then done
static UInt64 first_host;                // host time of the take's first sample
static mach_timebase_info_data_t timebase;
static double tick_seconds, max_late;
static long timed_buffers;

static OSStatus on_overload(AudioObjectID id, UInt32 n, const AudioObjectPropertyAddress *a,
                            void *ud) {
    overloads++;
    return noErr;
}

static void record_cb(void *ud, AudioQueueRef q, AudioQueueBufferRef b,
                      const AudioTimeStamp *t, UInt32 n, const AudioStreamPacketDescription *d) {
    if (__atomic_load_n(&done, __ATOMIC_RELAXED)) return;
    long frames = b->mAudioDataByteSize / BYTES_PER_FRAME;
    if (t && (t->mFlags & kAudioTimeStampHostTimeValid)) {
        double late = ((double)mach_absolute_time() - (double)t->mHostTime) * tick_seconds
                      - (double)frames / RATE;
        if (late > max_late) max_late = late;
        timed_buffers++;
    }
    if (frames_written + frames > frames_target) frames = frames_target - frames_written;
    if (frames > 0) {
        memcpy(take + frames_written * BYTES_PER_FRAME, b->mAudioData,
               (size_t)frames * BYTES_PER_FRAME);
        frames_written += frames;
    }
    if (!__atomic_load_n(&started, __ATOMIC_RELAXED)) {
        if (t && (t->mFlags & kAudioTimeStampHostTimeValid)) first_host = t->mHostTime;
        __atomic_store_n(&started, 1, __ATOMIC_RELEASE);
    }
    if (frames_written >= frames_target) {
        __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
        return;
    }
    AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

static int write_wav(FILE *f, long frames) {
    unsigned int data = (unsigned int)(frames * BYTES_PER_FRAME), riff = 36 + data;
    unsigned int sr = RATE, br = RATE * BYTES_PER_FRAME, fmt = 16;
    unsigned short pcm = 1, ch = CHANNELS, ba = BYTES_PER_FRAME, bits = 16;
    int ok = fwrite("RIFF", 1, 4, f) == 4 && fwrite(&riff, 4, 1, f) == 1 &&
             fwrite("WAVEfmt ", 1, 8, f) == 8 && fwrite(&fmt, 4, 1, f) == 1 &&
             fwrite(&pcm, 2, 1, f) == 1 && fwrite(&ch, 2, 1, f) == 1 &&
             fwrite(&sr, 4, 1, f) == 1 && fwrite(&br, 4, 1, f) == 1 &&
             fwrite(&ba, 2, 1, f) == 1 && fwrite(&bits, 2, 1, f) == 1 &&
             fwrite("data", 1, 4, f) == 4 && fwrite(&data, 4, 1, f) == 1 &&
             fwrite(take, BYTES_PER_FRAME, (size_t)frames, f) == (size_t)frames;
    return fclose(f) == 0 && ok;
}

static int record(const char *uid, double seconds, const char *path) {
    frames_target = lround(seconds * RATE);
    if (frames_target <= 0) { fprintf(stderr, "SECONDS must be positive\n"); return 2; }
    FILE *out = fopen(path, "wb");
    if (!out) { perror(path); return 1; }
    take = calloc((size_t)frames_target, BYTES_PER_FRAME);
    if (!take) { fprintf(stderr, "no memory for %.1f s\n", seconds); return 1; }
    mach_timebase_info(&timebase);
    tick_seconds = (double)timebase.numer / timebase.denom * 1e-9;

    CFStringRef uid_string = CFStringCreateWithCString(NULL, uid, kCFStringEncodingUTF8);
    AudioObjectID device = kAudioObjectUnknown;
    UInt32 size = sizeof device;
    AudioObjectPropertyAddress translate = {kAudioHardwarePropertyTranslateUIDToDevice,
                                            kAudioObjectPropertyScopeGlobal,
                                            kAudioObjectPropertyElementMain};
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &translate, sizeof uid_string,
                               &uid_string, &size, &device);
    CFRelease(uid_string);
    if (device == kAudioObjectUnknown) { fprintf(stderr, "no device with UID %s\n", uid); return 1; }
    AudioObjectPropertyAddress overload = {kAudioDeviceProcessorOverload,
                                           kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMain};
    if (AudioObjectAddPropertyListener(device, &overload, on_overload, NULL)) {
        fprintf(stderr, "cannot watch %s for overloads\n", uid);
        return 1;
    }

    AudioStreamBasicDescription f = pcm_format();
    AudioQueueRef q;
    OSStatus s = AudioQueueNewInput(&f, record_cb, NULL, NULL, NULL, 0, &q);
    if (s) { fprintf(stderr, "AudioQueueNewInput %d\n", (int)s); return 1; }
    if (use_device(q, uid)) return 1;
    for (int i = 0; i < 8; i++) {
        AudioQueueBufferRef b;
        if ((s = AudioQueueAllocateBuffer(q, BUFFER_FRAMES * BYTES_PER_FRAME, &b)) ||
            (s = AudioQueueEnqueueBuffer(q, b, 0, NULL))) {
            fprintf(stderr, "buffer setup %d\n", (int)s);
            return 1;
        }
    }
    if ((s = AudioQueueStart(q, NULL))) { fprintf(stderr, "AudioQueueStart %d\n", (int)s); return 1; }

    int announced = 0;
    long seen = -1, still_ms = 0;
    for (;;) {
        int finished = __atomic_load_n(&done, __ATOMIC_ACQUIRE);   // done first: a done
        int is_started = __atomic_load_n(&started, __ATOMIC_ACQUIRE); // take is started
        if (is_started && !announced) {
            printf("recording %llu\n",
                   (unsigned long long)first_host * timebase.numer / timebase.denom);
            fflush(stdout);
            announced = 1;
        }
        if (finished) break;
        long now = frames_written;
        still_ms = now == seen ? still_ms + 1 : 0;
        seen = now;
        if (still_ms > STALL_MS) {
            fprintf(stderr, is_started ? "%s stopped delivering audio\n"
                                       : "no audio arrived from %s\n", uid);
            return 1;
        }
        usleep(1000);
    }
    AudioQueueStop(q, true);
    AudioQueueDispose(q, true);
    AudioObjectRemovePropertyListener(device, &overload, on_overload, NULL);
    if (!write_wav(out, frames_written)) { perror(path); return 1; }

    if (overloads) {
        fprintf(stderr, "%s reported %d overload(s) while recording: the take has a hole\n",
                uid, overloads);
        return 3;
    }
    if (!timed_buffers) {
        fprintf(stderr, "%s gave no host times: whether the take has a hole is unknown\n", uid);
        return 3;
    }
    if (max_late > MAX_LATE) {
        fprintf(stderr, "the recorder fell %.2f s behind %s (limit %.2f s): the take may "
                        "have a hole\n", max_late, uid, MAX_LATE);
        return 3;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--list")) return list_devices();
    if (argc == 4 && argv[1][0] != '-') return record(argv[1], atof(argv[2]), argv[3]);
    fprintf(stderr, "usage: aqrec --list\n"
                    "       aqrec UID SECONDS OUT.wav\n");
    return 2;
}
