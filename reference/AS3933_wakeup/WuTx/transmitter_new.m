% Pattern-Based OOK SCM Wake-Up Transmitter for AS3933
% This version implements the wake-up protocol with pattern detection
clear; close all;

%% Configuration Parameters
centerFrequency = 433e6;        % HF carrier (433 MHz)
emulatedCarrierFreq = 19e3;     % LF envelope target (19 kHz, within 15-23kHz band)

%% AS3933 Protocol Requirements (from datasheet)
% For 15-23kHz band: minimum carrier burst = 92*Tclk + 8*Tcarr
% With 32.768kHz clock: Tclk = 30.5μs, so 92*30.5μs ≈ 2.8ms
% Plus 8 carrier periods: 8*(1/19kHz) ≈ 0.42ms
% Total minimum: ~3.2ms, we use 4ms for safety
carrierBurstDuration = 4e-3;    % 4ms carrier burst

% Manchester bit timing (from datasheet Figure 48)
% Using R7<4:0> = 01111 (11 decimal) = 15 clock periods per bit
% With 32.768kHz clock: bit duration = 15 * 30.5μs ≈ 457μs
clockFrequency = 33250;
clockPeriod = 1/ clockFrequency;
manchesterBitDuration = 32* clockPeriod;  % ≈ 976μs per Manchester bit
manchesterSymbolDuration = 2 * manchesterBitDuration; % ≈ 1953μs per symbol

%% Wake-up pattern and the encoded version of it
% Wake-up pattern (16-bit Manchester encoded)
% Default pattern from datasheet: R5=0x69, R6=0x96
% R6 (first byte) = 0x96 = 10010110 (MSB first)
% R5 (second byte) = 0x69 = 01101001 (MSB first)
% Combined 16-bit pattern: 1001011001101001
wakeupPattern = [1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1];

%% Manchester Encoding
% Manchester encoding: '1' = high-to-low, '0' = low-to-high
% Each symbol becomes 2 bits
manchesterBits = zeros(1, 2 * length(wakeupPattern));
manchesterBits(1:2:end) = wakeupPattern;           % First bit of each pair
manchesterBits(2:2:end) = ~wakeupPattern;          % Second bit (inverted)
%% Pluto SDR Configuration
samplesPerSubcarrierBit = 20;
sampleRate = emulatedCarrierFreq * samplesPerSubcarrierBit;


%% Generate Square Wave Carrier Waveform
% Generate time vectors
carrierBurstSamples = ceil(carrierBurstDuration * sampleRate);
manchesterBitSamples = ceil(manchesterBitDuration * sampleRate);

% Create time vectors
t_carrier = (0:carrierBurstSamples-1) / sampleRate;
t_manchester = (0:manchesterBitSamples-1) / sampleRate;

% Generate square wave carrier at emulated frequency (0 to 1 range)
carrier_square = (sign(sin(2 * pi * emulatedCarrierFreq * t_carrier)) + 1) / 2;

%% Generate Protocol Waveform with Square Wave Carrier
% Pre-generate waveform templates (0 to 1 range)
squareTemplate = (sign(sin(2 * pi * emulatedCarrierFreq * t_manchester)) + 1) / 2;
zeroTemplate = zeros(1, manchesterBitSamples);

% 1. Carrier Burst (continuous square wave)
carrierBurstWaveform = carrier_square;

% 2. Separation (half Manchester symbol = 1 bit duration)
separationWaveform = zeros(size(t_manchester));

% 3. Preamble (minimum 6 bits: 101010) - Vectorized
preambleBits = [1, 0, 1, 0, 1, 0]; % 6-bit preamble
preambleMatrix = [zeroTemplate; squareTemplate]; % Row 1=zeros(0), Row 2=carrier(1) 
preambleSelection = preambleMatrix(preambleBits + 1, :); % Select rows: 0→row1(zeros), 1→row2(carrier)
preambleWaveform = reshape(preambleSelection', 1, []); % Transpose and flatten

% 4. Manchester Encoded Pattern - Vectorized  
patternMatrix = [zeroTemplate; squareTemplate]; % Row 1=zeros(0), Row 2=carrier(1)
patternSelection = patternMatrix(manchesterBits + 1, :); % Select rows: 0→row1(zeros), 1→row2(carrier)
patternWaveform = reshape(patternSelection', 1, []); % Transpose and flatten
%% Combine All Parts
completeWaveform = [carrierBurstWaveform, separationWaveform, preambleWaveform, patternWaveform];

% Add some silence at the end
silenceDuration = 1e-3; % 1ms silence
silenceSamples = ceil(silenceDuration * sampleRate);
silence = zeros(1, silenceSamples);
completeWaveform = [completeWaveform, silence];

% Convert to complex for Pluto (real part contains the envelope)
completeComplex = complex(completeWaveform(:));

%% Plot the complete waveform
% Create time vector for the complete waveform
timeVector = (0:length(completeWaveform)-1) / sampleRate; % Time in seconds

% Plot the waveform
figure;
plot(timeVector * 1000, completeWaveform); % Time in milliseconds
xlabel('Time (ms)');
ylabel('Amplitude');
title('AS3933 Wake-Up Protocol Waveform');
grid on;

% Optional: Add markers for different sections
hold on;
carrierEndTime = length(carrierBurstWaveform) / sampleRate * 1000;
separationEndTime = (length(carrierBurstWaveform) + length(separationWaveform)) / sampleRate * 1000;
preambleEndTime = (length(carrierBurstWaveform) + length(separationWaveform) + length(preambleWaveform)) / sampleRate * 1000;

% Add vertical lines to mark sections
xline(carrierEndTime, 'r--', 'Carrier Burst');
xline(separationEndTime, 'g--', 'Separation');
xline(preambleEndTime, 'b--', 'Preamble');
legend('Waveform', 'Location', 'best');
hold off;

%% Display Protocol Information
fprintf('=== AS3933 Pattern-Based Wake-Up Protocol ===\n');
fprintf('Carrier frequency: %.1f MHz\n', centerFrequency/1e6);
fprintf('Emulated LF carrier: %.1f kHz\n', emulatedCarrierFreq/1e3);
fprintf('Carrier burst duration: %.1f ms\n', carrierBurstDuration*1e3);
fprintf('Manchester bit duration: %.0f μs\n', manchesterBitDuration*1e6);
fprintf('Wake-up pattern (hex): R6=0x96, R5=0x69\n');
fprintf('Wake-up pattern (bin): %s\n', sprintf('%d', wakeupPattern));
fprintf('Manchester bits: %s\n', sprintf('%d', manchesterBits));
fprintf('Total waveform duration: %.1f ms\n', length(completeWaveform)/sampleRate*1e3);
fprintf('Sample rate: %.1f kHz\n', sampleRate/1e3);

%% Pluto SDR Setup
% Transmission parameters
retransmissionDelay = 0.1; % seconds between complete protocol transmissions
try
    plutoTx = sdrtx('Pluto', ...
        'RadioID', 'usb:0', ...
        'CenterFrequency', centerFrequency, ...
        'BasebandSampleRate', sampleRate, ...
        'Gain', 0);
    

    
    fprintf('\nTransmitting pattern-based wake-up calls...\n');
    fprintf('Press Ctrl+C to stop\n');
    
    transmissionCount = 0;
    while true
        transmissionCount = transmissionCount + 1;
        fprintf('Transmission #%d at %s\n', transmissionCount, string(datetime('now', 'Format', 'HH:mm:ss')));
        
        % Transmit the complete protocol
        plutoTx(completeComplex);
        
        % Wait before next transmission
        pause(retransmissionDelay);
    end
    
catch ME
    fprintf('Error: %s\n', ME.message);
    if exist('plutoTx', 'var')
        release(plutoTx);
    end
    fprintf('Transmission stopped.\n');
end