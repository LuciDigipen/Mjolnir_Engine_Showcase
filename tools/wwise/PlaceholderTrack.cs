// Synthesizes SoundtrackCheck's placeholders. The track: 64 bars at 84 BPM, 48 kHz
// stereo, C - Am - F - G7 with a pad, bass, an arpeggio that pans across the field
// and a melody in the second and fourth sections. Every tail wraps to the start,
// so both files loop without a seam.
using System;
using System.IO;

public static class PlaceholderTrack
{
    const int SampleRate = 48000;
    const double Bpm = 84.0;
    const int Bars = 64;
    const double Beat = 60.0 / Bpm;
    const double Bar = 4.0 * Beat;

    static readonly int[][] Pads =
    {
        new[] { 60, 64, 67, 71 }, // Cmaj7
        new[] { 57, 60, 64, 67 }, // Am7
        new[] { 53, 57, 60, 64 }, // Fmaj7
        new[] { 55, 59, 62, 65 }, // G7
    };
    static readonly int[] Roots = { 36, 33, 29, 31 };

    static float[] sLeft;
    static float[] sRight;
    static int sFrames;

    static double Hz(int midi) { return 440.0 * Math.Pow(2.0, (midi - 69) / 12.0); }

    static void Add(double time, double left, double right)
    {
        int frame = (int)(time * SampleRate) % sFrames;
        sLeft[frame] += (float)left;
        sRight[frame] += (float)right;
    }

    static void PanGains(double pan, out double left, out double right)
    {
        double angle = (pan + 1.0) * Math.PI * 0.25;
        left = Math.Cos(angle);
        right = Math.Sin(angle);
    }

    static void Pad(double start, double length, int midi)
    {
        const double attack = 0.8, release = 1.2;
        double freq = Hz(midi);
        int count = (int)((length + release) * SampleRate);
        for (int i = 0; i < count; ++i)
        {
            double t = (double)i / SampleRate;
            double env = Math.Min(1.0, t / attack);
            if (t > length)
                env *= Math.Max(0.0, 1.0 - (t - length) / release);

            // detuned a few cents apart per side, which is what makes the pad wide
            double l = Math.Sin(2 * Math.PI * freq * 0.997 * t) + 0.25 * Math.Sin(2 * Math.PI * freq * 2 * 0.997 * t);
            double r = Math.Sin(2 * Math.PI * freq * 1.003 * t) + 0.25 * Math.Sin(2 * Math.PI * freq * 2 * 1.003 * t);
            Add(start + t, 0.035 * env * l, 0.035 * env * r);
        }
    }

    static void Pluck(double start, int midi, double gain, double pan, double decay)
    {
        double freq = Hz(midi);
        double gl, gr;
        PanGains(pan, out gl, out gr);
        int count = (int)(decay * 5.0 * SampleRate);
        for (int i = 0; i < count; ++i)
        {
            double t = (double)i / SampleRate;
            double env = Math.Exp(-t / decay) * Math.Min(1.0, t / 0.004);
            double s = Math.Sin(2 * Math.PI * freq * t) + Math.Sin(2 * Math.PI * freq * 3 * t) / 9.0;
            Add(start + t, gain * env * s * gl, gain * env * s * gr);
        }
    }

    static void Lead(double start, double length, int midi, double pan)
    {
        double freq = Hz(midi);
        double gl, gr;
        PanGains(pan, out gl, out gr);
        int count = (int)((length + 0.4) * SampleRate);
        for (int i = 0; i < count; ++i)
        {
            double t = (double)i / SampleRate;
            double env = Math.Min(1.0, t / 0.05);
            if (t > length)
                env *= Math.Max(0.0, 1.0 - (t - length) / 0.4);
            double vibrato = 1.0 + 0.004 * Math.Sin(2 * Math.PI * 5.0 * t) * Math.Min(1.0, t / 0.6);
            double s = Math.Sin(2 * Math.PI * freq * vibrato * t) + 0.15 * Math.Sin(4 * Math.PI * freq * vibrato * t);
            Add(start + t, 0.07 * env * s * gl, 0.07 * env * s * gr);
        }
    }

    static void Shaker(double start, double pan, Random random)
    {
        double gl, gr;
        PanGains(pan, out gl, out gr);
        int count = (int)(0.08 * SampleRate);
        double previous = 0.0;
        for (int i = 0; i < count; ++i)
        {
            double t = (double)i / SampleRate;
            double noise = random.NextDouble() * 2.0 - 1.0;
            double high = noise - previous;
            previous = noise;
            double env = Math.Exp(-t / 0.018);
            Add(start + t, 0.02 * env * high * gl, 0.02 * env * high * gr);
        }
    }

    static readonly int[][] Melody =
    {
        new[] { 76, 74, 72, 71 }, new[] { 72, 71, 69, 67 },
        new[] { 69, 72, 76, 74 }, new[] { 74, 71, 67, 71 },
    };

    public static double Seconds { get { return Bars * Bar; } }

    public static void Write(string path)
    {
        sFrames = (int)Math.Round(Bars * Bar * SampleRate);
        sLeft = new float[sFrames];
        sRight = new float[sFrames];
        Random random = new Random(550);

        for (int bar = 0; bar < Bars; bar += 2)
        {
            int chord = (bar / 2) % 4;
            int section = bar / 16;
            double start = bar * Bar;

            foreach (int midi in Pads[chord])
                Pad(start, 2 * Bar, midi);

            for (int beat = 0; beat < 8; beat += 2)
                Pluck(start + beat * Beat, Roots[chord], 0.22, 0.0, 0.35);

            int[] tones = Pads[chord];
            for (int step = 0; step < 16; ++step)
            {
                double time = start + step * Beat * 0.5;
                int index;
                if (section % 2 == 0)
                    index = step % 4;
                else
                    index = step % 8 < 4 ? step % 4 : 7 - step % 8;

                // one sweep from left to right and back every four bars
                double pan = 0.85 * Math.Sin(2 * Math.PI * time / (4 * Bar));
                Pluck(time, tones[index] + 12, 0.06, pan, 0.25);

                if (section >= 2 && step % 2 == 1)
                    Shaker(time, 0.5, random);
            }

            if (section == 1 || section == 3)
            {
                int[] line = Melody[chord];
                for (int note = 0; note < 4; ++note)
                    Lead(start + note * 2 * Beat, 2 * Beat * 0.9, line[note], -0.3);
            }
        }

        Save(path);
    }

    // The SFX placeholder: a 2 s loop of high blips that ping-pong between the
    // speakers, far enough from the music to tell the two volumes apart
    public static void WriteSfxLoop(string path)
    {
        sFrames = 2 * SampleRate;
        sLeft = new float[sFrames];
        sRight = new float[sFrames];

        int[] notes = { 84, 88, 91, 88 };
        for (int step = 0; step < 4; ++step)
            Pluck(step * 0.5, notes[step], 0.3, step % 2 == 0 ? -0.9 : 0.9, 0.08);

        Save(path);
    }

    static void Save(string path)
    {
        float peak = 0.0f;
        for (int i = 0; i < sFrames; ++i)
            peak = Math.Max(peak, Math.Max(Math.Abs(sLeft[i]), Math.Abs(sRight[i])));
        float scale = peak > 0.0f ? 0.7f / peak : 1.0f;

        using (BinaryWriter writer = new BinaryWriter(File.Create(path)))
        {
            int dataBytes = sFrames * 4;
            writer.Write(new[] { 'R', 'I', 'F', 'F' });
            writer.Write(36 + dataBytes);
            writer.Write(new[] { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
            writer.Write(16);
            writer.Write((short)1);
            writer.Write((short)2);
            writer.Write(SampleRate);
            writer.Write(SampleRate * 4);
            writer.Write((short)4);
            writer.Write((short)16);
            writer.Write(new[] { 'd', 'a', 't', 'a' });
            writer.Write(dataBytes);
            for (int i = 0; i < sFrames; ++i)
            {
                writer.Write((short)Math.Round(Math.Max(-1.0f, Math.Min(1.0f, sLeft[i] * scale)) * 32767.0f));
                writer.Write((short)Math.Round(Math.Max(-1.0f, Math.Min(1.0f, sRight[i] * scale)) * 32767.0f));
            }
        }
    }
}
