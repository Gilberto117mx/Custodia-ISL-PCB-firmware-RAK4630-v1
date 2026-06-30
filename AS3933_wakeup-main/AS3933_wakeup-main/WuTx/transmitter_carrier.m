% Updated OOK SCM Wake-Up for AS3933 (Frequency-only, 15–23kHz band)

clear; close all;

centerFrequency = 433e6;       % HF carrier
emulatedCarrierFreq = 19e3;    % LF envelope target
subcarrierBitRate = emulatedCarrierFreq * 2; % 38 kbps

% Burst duration: >= 4ms to ensure AGC settle + channel select
burstDuration = 6e-3; % 6ms

% Pluto SDR sample rate
samplesPerSubcarrierBit = 20;
sampleRate = subcarrierBitRate * samplesPerSubcarrierBit;

% Generate continuous LF burst
numSubcarrierBits = ceil(burstDuration * subcarrierBitRate);
if mod(numSubcarrierBits, 2) ~= 0, numSubcarrierBits = numSubcarrierBits + 1; end
subcarrierPattern = repmat([1 0], 1, numSubcarrierBits / 2);
burstWaveform = repelem(subcarrierPattern, samplesPerSubcarrierBit);

% Convert to complex for Pluto
burstComplex = complex(burstWaveform(:));

% Pluto TX object
plutoTx = sdrtx('Pluto', ...
    'RadioID',           'usb:0', ...
    'CenterFrequency',   centerFrequency, ...
    'BasebandSampleRate', sampleRate, ...
    'Gain',              0);

% Transmission loop
retransmissionDelay = 2; % seconds between bursts
disp('Transmitting continuous LF bursts for AS3933 frequency-only wake-up...');
try
    while true
        plutoTx(burstComplex);
        pause(retransmissionDelay);
    end
catch
    release(plutoTx);
    disp('Stopped transmission.');
end
