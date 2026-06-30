% Simple AS3933 Wake-Up Signal Receiver - Real-Time Plotting
% Receives at 433 MHz and displays the signal in real-time
clear; close all;

%% Configuration Parameters (matching transmitter)
centerFrequency = 433e6;        % 433 MHz (same as transmitter)
sampleRate = 380e3;             % 380 kHz sample rate
timeFrameDuration = 100e-3;     % 100ms time frame
bufferSamples = ceil(timeFrameDuration * sampleRate); % Samples for 100ms
receiverGain = 70;              % Receiver gain in dB

%% Initialize Real-Time Plot
fig = figure('Name', 'Real-Time Signal Receiver - 433 MHz', 'Position', [100 100 1000 650]);

% Create control structure to hold state variables and store in figure
controls = struct();
controls.stopRequested = false;
controls.pauseRequested = false;
set(fig, 'UserData', controls);

% Add Stop Button
stopButton = uicontrol('Style', 'pushbutton', 'String', 'STOP', ...
    'Position', [350 10 100 40], 'FontSize', 14, 'FontWeight', 'bold', ...
    'BackgroundColor', [0.8 0.2 0.2], 'ForegroundColor', 'white', ...
    'Callback', {@stopCallback, fig});

% Add Pause/Resume Button  
pauseButton = uicontrol('Style', 'pushbutton', 'String', 'PAUSE', ...
    'Position', [470 10 100 40], 'FontSize', 14, 'FontWeight', 'bold', ...
    'BackgroundColor', [0.9 0.6 0.1], 'ForegroundColor', 'white', ...
    'Callback', {@pauseCallback, fig});


% Time domain plot (mV)
subplot(2,2,1);
timeAxis = (0:bufferSamples-1) / sampleRate * 1000; % Time in milliseconds
h_signal = plot(timeAxis, zeros(bufferSamples,1), 'b-', 'LineWidth', 1);
xlabel('Time (ms)');
ylabel('Amplitude (mV)');
title('Received Signal - Amplified (mV)');
grid on;
ylim([-1000 1000]);  % Range for millivolts
xlim([0 max(timeAxis)]);

% Real part plot (I component)
subplot(2,2,2);
timeAxis2 = (0:bufferSamples-1) / sampleRate * 1000; % Time in milliseconds for second plot
h_real = plot(timeAxis2, zeros(bufferSamples,1), 'r-', 'LineWidth', 1);
xlabel('Time (ms)');
ylabel('Real Part (mV)');
title('I Component (Real Part)');
grid on;
ylim([-1000 1000]);  % Range for millivolts
xlim([0 max(timeAxis2)]);

% Frequency spectrum plot (FFT)
subplot(2,2,3);
freqAxis = (0:bufferSamples/2-1) / (bufferSamples/2) * (sampleRate/2) / 1e3; % Frequency in kHz
h_spectrum = plot(freqAxis, zeros(bufferSamples/2,1), 'g-', 'LineWidth', 1);
xlabel('Frequency (kHz)');
ylabel('PSD (dBm/Hz)');
title('Power Spectral Density');
grid on;
ylim([-140 -40]);  % Adjusted for dBm/Hz scale
xlim([0 max(freqAxis)]);

% Imaginary part plot (Q component)
subplot(2,2,4);
timeAxis3 = (0:bufferSamples-1) / sampleRate * 1000; % Time in milliseconds for fourth plot
h_imag = plot(timeAxis3, zeros(bufferSamples,1), 'm-', 'LineWidth', 1);
xlabel('Time (ms)');
ylabel('Imaginary Part (mV)');
title('Q Component (Imaginary Part)');
grid on;
ylim([-1000 1000]);  % Range for millivolts
xlim([0 max(timeAxis3)]);

%% Initialize Pluto SDR Receiver
fprintf('=== Simple 433 MHz Receiver ===\n');
fprintf('Center frequency: %.1f MHz\n', centerFrequency/1e6);
fprintf('Sample rate: %.1f kHz\n', sampleRate/1e3);
fprintf('Samples per frame: %d\n', bufferSamples);
fprintf('Frame duration: %.2f ms\n', bufferSamples/sampleRate*1000);

try
    % Initialize Pluto SDR receiver
    plutoRx = sdrrx('Pluto', ...
        'RadioID', 'usb:0', ...
        'CenterFrequency', centerFrequency, ...
        'BasebandSampleRate', sampleRate, ...
        'SamplesPerFrame', bufferSamples, ...
        'GainSource', 'Manual', ...
        'Gain', receiverGain, ...
        'OutputDataType', 'double');
    
    fprintf('\nReceiver initialized successfully!\n');
    fprintf('Receiving signals... Press Ctrl+C to stop\n\n');
    
    frameCount = 0;
    startTime = tic; % Start timing for continuous time axis
    
    % Main reception loop
    while true
        % Get current controls state from figure
        controls = get(fig, 'UserData');
        
        % Check if stop was requested
        if controls.stopRequested
            break;
        end
        
        % Check if paused
        if controls.pauseRequested
            pause(0.1); % Wait 100ms when paused
            drawnow; % Process GUI events including button clicks
            continue; % Skip the rest of this iteration
        end
        
        frameCount = frameCount + 1;
        
        % Receive samples from Pluto SDR
        [rxData, ~, overrun] = plutoRx();
        
        if overrun
            fprintf('Warning: Data overrun at frame %d\n', frameCount);
        end
        
        % Get both I and Q components for analysis
        rxSignal_I = real(rxData);
        rxSignal_Q = imag(rxData);
        rxSignal_magnitude = abs(rxData);  % Envelope/magnitude
        
        % Use magnitude for main analysis (this should show the envelope)
        rxSignal = rxSignal_magnitude;
        
        % Convert amplitude to mV (rough conversion for Pluto SDR)
        % Pluto SDR typically has ~0.5V peak-to-peak full scale
        plutoFullScaleVolts = 0.5;  % Adjust based on your Pluto calibration (1V = full scale)
        rxSignal_mV = rxSignal * plutoFullScaleVolts * 1000; % Convert to millivolts
        
        % Convert I and Q components to mV
        rxSignal_I_mV = rxSignal_I * plutoFullScaleVolts * 1000; % I component in mV
        rxSignal_Q_mV = rxSignal_Q * plutoFullScaleVolts * 1000; % Q component in mV
        
        % Convert amplitude to dBm (without subtracting receiver gain)
        plutoFullScaledBm = 0;  % Adjust this based on your Pluto calibration
        rxSignal_dBm = 20*log10(abs(rxSignal) + eps) + plutoFullScaledBm;
        
        % Calculate FFT for spectrum with proper units
        fftData = fft(rxData);
        % Normalize by length and apply window correction
        fftMagnitude = abs(fftData(1:bufferSamples/2)) / (bufferSamples/2);
        % Convert to Power Spectral Density in dBm/Hz
        % PSD = V^2 / (R * BW) where R=50Ω, BW=sample_rate, convert to mW
        spectrum_PSD = 10 * log10((fftMagnitude.^2 * plutoFullScaleVolts^2 / (50 * sampleRate)) * 1000 + eps);
        spectrum = spectrum_PSD;
        
        % Update time axis for continuous scrolling time
        currentTime = toc(startTime) * 1000; % Current time in milliseconds
        timeAxisLive = currentTime + (0:bufferSamples-1) / sampleRate * 1000;
        
        % Update all four plots: magnitude, real, FFT spectrum, and imaginary
        set(h_signal, 'XData', timeAxisLive, 'YData', rxSignal_mV);
        set(h_real, 'XData', timeAxisLive, 'YData', rxSignal_I_mV);
        set(h_spectrum, 'YData', spectrum);
        set(h_imag, 'XData', timeAxisLive, 'YData', rxSignal_Q_mV);
        
        % Update x-axis limits to show 100ms time window for time domain plots
        subplot(2,2,1);
        xlim([currentTime, currentTime + 100]);
        subplot(2,2,2);
        xlim([currentTime, currentTime + 100]);
        subplot(2,2,4);
        xlim([currentTime, currentTime + 100]);
        
        % Update signal statistics for all components
        signalRMS_mV = rms(rxSignal_mV);
        signalMax_mV = max(abs(rxSignal_mV));
        signalRMS_I_mV = rms(rxSignal_I_mV);
        signalMax_I_mV = max(abs(rxSignal_I_mV));
        signalRMS_Q_mV = rms(rxSignal_Q_mV);
        signalMax_Q_mV = max(abs(rxSignal_Q_mV));
        
        subplot(2,2,1);
        title(sprintf('Frame %d - Magnitude: RMS=%.1f mV, Peak=%.1f mV', frameCount, signalRMS_mV, signalMax_mV));
        subplot(2,2,2);
        title(sprintf('I Component: RMS=%.1f mV, Peak=%.1f mV', signalRMS_I_mV, signalMax_I_mV));
        subplot(2,2,3);
        title('Power Spectral Density');
        subplot(2,2,4);
        title(sprintf('Q Component: RMS=%.1f mV, Peak=%.1f mV', signalRMS_Q_mV, signalMax_Q_mV));
        
        % Force plot update
        drawnow;
        
        % Print status every 50 frames
        if mod(frameCount, 50) == 0
            fprintf('Frame %d: Magnitude[RMS=%.1f mV] | I[RMS=%.1f mV] | Q[RMS=%.1f mV]\n', ...
                frameCount, signalRMS_mV, signalRMS_I_mV, signalRMS_Q_mV);
        end
    end
    
catch ME
    fprintf('\nError: %s\n', ME.message);
    
    % Clean up
    if exist('plutoRx', 'var')
        release(plutoRx);
        fprintf('Pluto SDR receiver released.\n');
    end
    
    % Get final controls state
    controls = get(fig, 'UserData');
    if controls.stopRequested
        fprintf('Reception stopped by user.\n');
    else
        fprintf('Reception stopped.\n');
    end
end

%% Callback Functions
function stopCallback(~, ~, fig)
    controls = get(fig, 'UserData');
    controls.stopRequested = true;
    set(fig, 'UserData', controls);
    fprintf('\nStop button pressed - stopping receiver...\n');
end

function pauseCallback(hObject, ~, fig)
    controls = get(fig, 'UserData');
    controls.pauseRequested = ~controls.pauseRequested;
    set(fig, 'UserData', controls);
    
    if controls.pauseRequested
        set(hObject, 'String', 'RESUME', 'BackgroundColor', [0.2 0.7 0.2]);
        fprintf('\nReceiver PAUSED - click RESUME to continue...\n');
    else
        set(hObject, 'String', 'PAUSE', 'BackgroundColor', [0.9 0.6 0.1]);
        fprintf('\nReceiver RESUMED\n');
    end
end
