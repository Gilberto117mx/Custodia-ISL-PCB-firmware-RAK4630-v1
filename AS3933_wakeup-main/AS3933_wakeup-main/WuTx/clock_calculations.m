fcarrier = 19e3;
fRC= fcarrier* (14/8); % for clock between 15 and 23 khz
TRC = 1/fRC
% Calculate the period in microseconds
TRC_microseconds = TRC * 1e6; % Convert seconds to microseconds
