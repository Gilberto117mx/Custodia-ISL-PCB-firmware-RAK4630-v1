% OOK Signal Generation for AS3933 using Subcarrier Modulation (SCM)
% This script generates a wake-up call (WuC) for the AS3933 chip.
% It uses a high-frequency carrier (450 MHz) and emulates the required
% low-frequency signal (19 kHz) using a subcarrier OOK method.

clear; % Clear workspace variables
close all;

% --- System & SCM Parameters ---
centerFrequency = 450e6;        % 450 MHz high-frequency carrier
retransmissionDelay = 0.01;      % 500 ms delay between transmissions

% --- AS3933 & Wake-Up Call (WuC) Parameters ---
% The AS3933's lowest frequency band is 15-23 kHz. We target the center.
emulatedCarrierFreq = 19e3;     % 19 kHz, center of the 15-23 kHz band

% To emulate the 19 kHz carrier with OOK, we need an alternating 1/0
% pattern. The bit rate of this subcarrier must be 2x the desired frequency.
subcarrierBitRate = emulatedCarrierFreq * 2; % 38 kbps

% The actual data/pattern is transmitted at a much lower rate.
% This is the bit rate for the preamble and the wake-up pattern itself.
messageBitRate = 2e3;           % 2 kbps for the wake-up pattern

% Wake-up pattern to be transmitted (example pattern)
wakeUpPattern = [1 0 0 1 0 1 1 0 0 1 1 0 1 0 0 1]; % 16-bit pattern
preamble = [1 0 1 0 1 0]; % Required 6-bit preamble (101010)
separationBit = 0; % A single '0' bit after the carrier burst

% --- ADALM-Pluto Configuration ---
% The sample rate must be high enough to accurately represent the
% 38 kHz subcarrier signal.
samplesPerSubcarrierBit = 20;
sampleRate = subcarrierBitRate * samplesPerSubcarrierBit; % 760 kHz

% Create a transmitter System object for the ADALM-Pluto radio
plutoTx = sdrtx('Pluto', ...
    'RadioID',           'usb:0', ...
    'CenterFrequency',   centerFrequency, ...
    'BasebandSampleRate', sampleRate, ...
    'Gain',              0);

% Create a receiver object to match the Tx sample rate (required by Pluto)
plutoRx = sdrrx('Pluto', ...
    'RadioID',            'usb:0', ...
    'CenterFrequency',    centerFrequency, ...
    'BasebandSampleRate', sampleRate, ...
    'SamplesPerFrame',    1024, ...
    'OutputDataType',     'double');

% --- Calculate Optimal Carrier Burst Duration (from AS3933 Datasheet) ---
% For the 15-23 kHz band, the formula is: Duration = 92*T_clk + 8*T_carr
% T_carr is the period of the emulated carrier (1 / 19 kHz)
T_carr = 1 / emulatedCarrierFreq;
% T_clk is the period of the AS3933's internal clock.
% For this band, f_clk = f_carr * (14/8)
f_clk = emulatedCarrierFreq * (14 / 8);
T_clk = 1 / f_clk;
% Calculate the minimum required duration in seconds
carrierBurstDuration = 92 * T_clk + 8 * T_carr;

fprintf('AS3933 Wake-Up Signal Configuration:\n');
fprintf('  - HF Carrier: %.2f MHz\n', centerFrequency / 1e6);
fprintf('  - Emulated LF Carrier: %d kHz\n', emulatedCarrierFreq / 1e3);
fprintf('  - Optimal Carrier Burst Duration: %.2f ms\n', carrierBurstDuration * 1000);

% --- Generate the Complete Baseband Waveform ---

% 1. Carrier Burst Waveform
% This is the emulated 19 kHz carrier (alternating 1s and 0s)
numBurstBits = ceil(carrierBurstDuration * subcarrierBitRate);
% Ensure it's an even number to end on a complete cycle
if mod(numBurstBits, 2) ~= 0, numBurstBits = numBurstBits + 1; end
burstSignal = repmat([1 0], 1, numBurstBits / 2);
carrierBurstWaveform = repelem(burstSignal, samplesPerSubcarrierBit);

% 2. Preamble, Separation Bit, and Pattern Waveform
% These are transmitted at the slower messageBitRate
samplesPerMessageBit = sampleRate / messageBitRate;
messageSignal = [separationBit, preamble, wakeUpPattern];
messageWaveform = [];
for bit = messageSignal
    if bit == 1
        % For a '1', we transmit the 19kHz subcarrier
        numSubcarrierBits = ceil( (1/messageBitRate) * subcarrierBitRate );
        if mod(numSubcarrierBits, 2) ~= 0, numSubcarrierBits = numSubcarrierBits + 1; end
        subcarrier_segment = repmat([1 0], 1, numSubcarrierBits / 2);
        messageWaveform = [messageWaveform, repelem(subcarrier_segment, samplesPerSubcarrierBit)];
    else
        % For a '0', we transmit nothing (zeros)
        messageWaveform = [messageWaveform, zeros(1, samplesPerMessageBit)];
    end
end

% 3. Combine all parts to form the final signal
final_waveform = [carrierBurstWaveform, messageWaveform];

% Convert the real waveform to a complex waveform for the Pluto SDR
final_waveform_complex = complex(final_waveform(:));

% --- Transmission Loop ---
fprintf('\nStarting transmission loop...\n');
fprintf('Press Ctrl+C to stop.\n');
try
    while true
        % Transmit the complete SCM wake-up signal
        plutoTx(final_waveform_complex);
        
        % Wait for the specified delay before retransmitting
        pause(retransmissionDelay);
    end
catch ME
    % Release hardware resources on error or user interruption
    release(plutoTx);
    release(plutoRx);
    
    fprintf('\nTransmission stopped and hardware released.\n');
    if ~strcmp(ME.identifier, 'MATLAB:class:OperationInterrupted')
       rethrow(ME);
    end
end
