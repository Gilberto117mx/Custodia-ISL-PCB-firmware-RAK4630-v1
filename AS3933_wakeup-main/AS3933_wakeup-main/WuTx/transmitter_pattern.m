% Pattern-Based OOK SCM Wake-Up Transmitter for AS3933
% This version implements the proper wake-up protocol with pattern detection
clear; close all;

%% Configuration Parameters
centerFrequency = 433e6;        % HF carrier (450 MHz)
emulatedCarrierFreq = 19e3;     % LF envelope target (19 kHz, within 15-23kHz band)
subcarrierBitRate = emulatedCarrierFreq * 2; % 38 kbps for alternating 1,0 pattern

% AS3933 Protocol Requirements (from datasheet)
% For 15-23kHz band: minimum carrier burst = 92*Tclk + 8*Tcarr
% With 32.768kHz clock: Tclk = 30.5μs, so 92*30.5μs ≈ 2.8ms
% Plus 8 carrier periods: 8*(1/19kHz) ≈ 0.42ms
% Total minimum: ~3.2ms, we use 4ms for safety
carrierBurstDuration = 4e-3;    % 4ms carrier burst

% Manchester bit timing (from datasheet Figure 48)
% Using R7<4:0> = 01011 (11 decimal) = 11 clock periods per bit
% With 32.768kHz clock: bit duration = 11 * 30.5μs ≈ 336μs
manchesterBitDuration =    4.5776e-04;  % 336μs per Manchester bit
manchesterSymbolDuration = 2 * manchesterBitDuration; % 672μs per symbol

% Wake-up pattern (16-bit Manchester encoded)
% Default pattern from datasheet: R5=0x69, R6=0x96
% R6 (first byte) = 0x96 = 10010110 (MSB first)
% R5 (second byte) = 0x69 = 01101001 (MSB first)
% Combined 16-bit pattern: 1001011001101001
wakeupPattern = [1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1];

%% Manchester Encoding
% Manchester encoding: '1' = high-to-low, '0' = low-to-high
% Each symbol becomes 2 bits
manchesterBits = [];
for i = 1:length(wakeupPattern)
    if wakeupPattern(i) == 1
        manchesterBits = [manchesterBits, 1, 0]; % High-to-low for '1'
    else
        manchesterBits = [manchesterBits, 0, 1]; % Low-to-high for '0'
    end
end

%% Pluto SDR Configuration
samplesPerSubcarrierBit = 20;
sampleRate = subcarrierBitRate * samplesPerSubcarrierBit;

%% Generate Protocol Waveform
% 1. Carrier Burst
numCarrierBits = ceil(carrierBurstDuration * subcarrierBitRate);
if mod(numCarrierBits, 2) ~= 0, numCarrierBits = numCarrierBits + 1; end
carrierPattern = repmat([1, 0], 1, numCarrierBits / 2);
carrierBurstWaveform = repelem(carrierPattern, samplesPerSubcarrierBit);

% 2. Separation Bit (half Manchester symbol = 1 bit duration)
separationDuration = manchesterBitDuration;
numSeparationBits = ceil(separationDuration * subcarrierBitRate);
if mod(numSeparationBits, 2) ~= 0, numSeparationBits = numSeparationBits + 1; end
separationPattern = repmat([0, 0], 1, numSeparationBits / 2);
separationWaveform = repelem(separationPattern, samplesPerSubcarrierBit);

% 3. Preamble (minimum 6 bits: 101010)
preambleBits = [1, 0, 1, 0, 1, 0]; % 6-bit preamble
preambleWaveform = [];
for bit = preambleBits
    bitDuration = manchesterBitDuration;
    numBitsForThisBit = ceil(bitDuration * subcarrierBitRate);
    if mod(numBitsForThisBit, 2) ~= 0, numBitsForThisBit = numBitsForThisBit + 1; end
    
    if bit == 1
        bitPattern = repmat([1, 0], 1, numBitsForThisBit / 2);
    else
        bitPattern = zeros(1, numBitsForThisBit); % No carrier for '0'
    end
    
    preambleWaveform = [preambleWaveform, repelem(bitPattern, samplesPerSubcarrierBit)];
end

% 4. Manchester Encoded Pattern
patternWaveform = [];
for bit = manchesterBits
    bitDuration = manchesterBitDuration;
    numBitsForThisBit = ceil(bitDuration * subcarrierBitRate);
    if mod(numBitsForThisBit, 2) ~= 0, numBitsForThisBit = numBitsForThisBit + 1; end
    
    if bit == 1
        bitPattern = repmat([1, 0], 1, numBitsForThisBit / 2);
    else
        bitPattern = zeros(1, numBitsForThisBit); % No carrier for '0'
    end
    
    patternWaveform = [patternWaveform, repelem(bitPattern, samplesPerSubcarrierBit)];
end

%% Combine All Parts
completeWaveform = [carrierBurstWaveform, separationWaveform, preambleWaveform, patternWaveform];

% Add some silence at the end
silenceDuration = 1e-3; % 1ms silence
silenceSamples = ceil(silenceDuration * sampleRate);
silence = zeros(1, silenceSamples);
completeWaveform = [completeWaveform, silence];

% Convert to complex for Pluto
completeComplex = complex(completeWaveform(:));

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
try
    plutoTx = sdrtx('Pluto', ...
        'RadioID', 'usb:0', ...
        'CenterFrequency', centerFrequency, ...
        'BasebandSampleRate', sampleRate, ...
        'Gain', 0);
    
    % Transmission parameters
    retransmissionDelay = 0.01; % seconds between complete protocol transmissions
    
    fprintf('\nTransmitting pattern-based wake-up calls...\n');
    fprintf('Press Ctrl+C to stop\n');
    
    transmissionCount = 0;
    while true
        transmissionCount = transmissionCount + 1;
        fprintf('Transmission #%d at %s\n', transmissionCount, datestr(now, 'HH:MM:SS'));
        
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