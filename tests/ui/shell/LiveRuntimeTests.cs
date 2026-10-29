using Xonar.Contracts.V1;
using Xonar.ControlCenter.Services;

namespace Xonar.ControlCenter.Tests;

public static class LiveRuntimeTests
{
    private static LiveAudioDevice Device(string id = "hardware-test") => new(id,
        new() { ChannelCount = 6, ChannelMask = 0x60f, SampleRate = 48000, SampleFormat = SampleFormat.Float32 },
        new() { Kind = DriverKind.Xonarse, Version = "test-driver" }, true);

    public static void RunAll()
    {
        Test.Run("Diagnostic réel : absence de flux sans instance inventée", NoInventedState);
        Test.Run("Diagnostic réel : formats simultanés stéréo et 5.1", SimultaneousFormats);
        Test.Run("Récupération : retrait, retour et nouveau GUID matériel", Recovery);
        Test.Run("Acquittement : tous les flux doivent appliquer le profil", SourceAcknowledgement);
        Test.Run("Écriture refusée : révision conservée", FailedWriteRevision);
        Test.Run("Marge : conseil mesuré, borné, sans atténuation répétée", Headroom);
        Test.Run("Export réel : formats et marge sans identité de périphérique", Export);
        Test.Run("Format incompatible : commandes DSP désactivées", IncompatibleFormat);
        Test.Run("Contrôle local : version et endpoint inconnus refusés", UnknownRequests);
    }

    private static void NoInventedState()
    {
        var broker = new LiveApoBroker(Path.Combine(Path.GetTempPath(), Guid.NewGuid()+".cfg"), () => null, () => null);
        var state = broker.BuildState();
        Test.Assert(!broker.IsConnected);
        Test.Equal(0, state.Instances.Count);
        Test.Assert(state.Meters is null && state.NegotiatedFormat is null);
        Test.Equal(JackReliability.Unknown, state.Jack.Reliability);
        Test.Equal(0, broker.GetCapabilitiesAsync().Result.Endpoints.Count);
    }

    private static void SimultaneousFormats()
    {
        var status = new DspLiveStatus { Pid = 123, Instance = 9, Channels = 6, InputMask = 0x60f, SampleRate = 48000,
            OutputPeak = new float[] { .1f,.1f,.2f,0,0,0 }, Sources = new[] {
                new DspLiveStatus.Source { Instance=1, InputChannels=2,InputMask=3,SampleRate=48000,Enabled=true },
                new DspLiveStatus.Source { Instance=2, InputChannels=6,InputMask=0x60f,SampleRate=48000,Enabled=true } } };
        var broker = new LiveApoBroker(Path.Combine(Path.GetTempPath(), Guid.NewGuid()+".cfg"), () => Device(), () => status);
        var state = broker.BuildState();
        Test.Equal(3, state.Instances.Count);
        Test.Equal(2U, state.Instances[1].InputFormat.ChannelCount);
        Test.Equal(6U, state.Instances[2].InputFormat.ChannelCount);
        Test.Equal("test-driver", state.Driver.Version);
        Test.Equal(6, state.Meters.OutputPeakDbfs.Length);
    }

    private static void Recovery()
    {
        LiveAudioDevice? current = Device();
        long now = 1000;
        DspLiveStatus? status = null;
        var broker = new LiveApoBroker(Path.Combine(Path.GetTempPath(), Guid.NewGuid()+".cfg"), () => current, () => status, () => now);
        int events=0;
        broker.ConnectionChanged += (_,_) => ++events;
        Test.Assert(!broker.RefreshHardware());
        current = null;Test.Assert(!broker.RefreshHardware() && !broker.IsConnected);
        Test.Equal(0, broker.GetCapabilitiesAsync().Result.Endpoints.Count);
        current = Device("new-endpoint-guid");
        Test.Assert(broker.RefreshHardware() && broker.IsConnected);
        Test.Assert(!broker.RefreshHardware(), "Aucune réapplication répétée sur matériel stable.");
        current = Device("next-endpoint-guid");
        Test.Assert(broker.RefreshHardware());
        Test.Equal(2, events);
        now += 6000;
        Test.Assert(broker.RefreshHardware(), "Une reprise après interruption du minuteur doit réouvrir les interfaces.");
        status = new() { Pid = 100 };
        Test.Assert(!broker.RefreshHardware());
        status = new() { Pid = 101 };
        Test.Assert(broker.RefreshHardware(), "Un nouvel audiodg doit déclencher la réapplication du profil.");
    }

    private static void SourceAcknowledgement()
    {
        string dir = Path.Combine(Path.GetTempPath(),"xonar-runtime-"+Guid.NewGuid());
        string cfg = Path.Combine(dir,"apo.cfg");
        DspLiveStatus? status = null;
        var broker = new LiveApoBroker(cfg, () => Device(), () => status);
        try {
            var response = broker.ApplyProfileAsync("MI_00",1,ProfileDefaults.CreateConservative()).GetAwaiter().GetResult();
            Test.Equal(ResponseStatus.Committed,response.Status);
            long time=File.GetLastWriteTimeUtc(cfg).ToFileTimeUtc();
            status = new() { ConfigFileTime=time, Sources=new[] { new DspLiveStatus.Source { ConfigFileTime=time-1 } } };
            Test.Assert(!broker.IsLastProfileApplied());
            status.Sources[0].ConfigFileTime=time;
            Test.Assert(broker.IsLastProfileApplied());
        }finally{if(Directory.Exists(dir))Directory.Delete(dir,true);}
    }

    private static void FailedWriteRevision()
    {
        string file=Path.GetTempFileName();
        try {
            var broker=new LiveApoBroker(Path.Combine(file,"apo.cfg"),()=>Device(),()=>null);
            var response=broker.ApplyProfileAsync("MI_00",1,ProfileDefaults.CreateConservative()).GetAwaiter().GetResult();
            Test.Equal(ResponseStatus.Error,response.Status);
            Test.Equal(1UL,broker.BuildState().Revision);
        }finally{File.Delete(file);}
    }

    private static void Headroom()
    {
        var status=new DspLiveStatus { PreLimiterPeak=1.5f,ClippedSamples=123 };
        var advice=HeadroomAdvice.From(status);
        Test.Assert(advice.AdjustmentDb < -4.5 && advice.AdjustmentDb > -4.6);
        Test.Assert(advice.ApplyTo(-59) == -60);
        Test.Equal(1.5f,status.PreLimiterPeak,"Le conseil ne doit modifier ni mesure ni profil.");
        status.PreLimiterPeak=.1f;
        Test.Equal(0.0,HeadroomAdvice.From(status).AdjustmentDb,"Les anciens clips ne déclenchent pas une seconde atténuation.");
        status.PreLimiterPeak=.5f;status.LimiterReductionDb=6;
        Test.Equal(-7.0,HeadroomAdvice.From(status).AdjustmentDb,"La réduction du limiteur doit être compensée en une seule proposition.");
        status.PreLimiterPeak=float.NaN;
        Test.Equal(0.0,HeadroomAdvice.From(status).AdjustmentDb);
        Test.Equal(0.0,HeadroomAdvice.From(null).AdjustmentDb);
    }

    private static void Export()
    {
        var broker=new LiveApoBroker(Path.Combine(Path.GetTempPath(), Guid.NewGuid()+".cfg"),()=>Device("private-hardware-id"),()=>null);
        string json=DiagnosticsExporter.Export(broker.BuildState(),new() { PreLimiterPeak=.3f,Sources=new[] {new DspLiveStatus.Source {InputChannels=2,InputMask=3,SampleRate=48000}}});
        Test.Assert(json.Contains("preLimiterPeak") && json.Contains("channels"));
        Test.Assert(!json.Contains("private-hardware-id"));
    }

    private static void IncompatibleFormat()
    {
        var device = Device() with { Format = new() { ChannelCount = 2, ChannelMask = 3, SampleRate = 48000, SampleFormat = SampleFormat.Float32 } };
        string path = Path.Combine(Path.GetTempPath(), Guid.NewGuid()+".cfg");
        var broker = new LiveApoBroker(path, () => device, () => null);
        var caps = broker.GetCapabilitiesAsync().Result;
        Test.Assert(!caps.Endpoints[0].Writable && !caps.Features.AutoDeclaredLayout);
        var response = broker.ApplyProfileAsync("MI_00", 1, ProfileDefaults.CreateConservative()).GetAwaiter().GetResult();
        Test.Equal(ResponseStatus.Error, response.Status);
        Test.Assert(!File.Exists(path));
    }

    private static void UnknownRequests()
    {
        var broker = new LiveApoBroker(Path.Combine(Path.GetTempPath(), Guid.NewGuid()+".cfg"), () => Device(), () => null);
        var request = new ControlRequest { ApiMajor = 99, RequestId = "test-request", Command = Command.GetCapabilities };
        var response = broker.SendAsync(request).GetAwaiter().GetResult();
        Test.Equal(ResponseStatus.Error, response.Status);
        Test.Equal("test-request", response.RequestId);
        request.ApiMajor = 1; request.Command = Command.GetState; request.EndpointKey = "other-hardware";
        response = broker.SendAsync(request).GetAwaiter().GetResult();
        Test.Equal(ErrorCode.EndpointUnavailable, response.Error.Code);
    }
}
