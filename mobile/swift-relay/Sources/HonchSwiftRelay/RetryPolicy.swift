import Foundation

public enum RetryPolicy {
    public static func nextBackoffDelayMs(attempt: Int, random: () -> Double = { Double.random(in: 0..<1) }) -> Int64 {
        let cappedAttempt = min(max(attempt, 0), 10)
        let base = Double(1 << cappedAttempt) * 1_000.0
        let jitter = 0.5 + random()
        return Int64(min(base * jitter, 60_000.0).rounded())
    }

    public static func parseRetryAfterMs(_ value: String?, now: Date = Date()) -> Int64? {
        guard let value else {
            return nil
        }

        if let seconds = Double(value), seconds >= 0 {
            return Int64((seconds * 1_000.0).rounded())
        }

        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = TimeZone(secondsFromGMT: 0)
        formatter.dateFormat = "EEE',' dd MMM yyyy HH':'mm':'ss z"
        guard let date = formatter.date(from: value) else {
            return nil
        }
        return max(0, Int64(date.timeIntervalSince(now) * 1_000.0))
    }
}
