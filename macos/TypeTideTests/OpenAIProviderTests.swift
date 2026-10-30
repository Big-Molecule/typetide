import XCTest
@testable import TypeTide

final class OpenAIProviderTests: XCTestCase {
    func testMissingAPIKeyFailsBeforeNetworkRequest() async {
        let provider = OpenAIProvider(baseURL: "https://example.com/v1", apiKey: "", model: "test")

        do {
            for try await _ in provider.stream(.init(text: "hello", source: .english, target: .chinese)) {}
            XCTFail("Expected missing-key error")
        } catch let error as TranslationError {
            XCTAssertEqual(error.errorDescription, "Missing OpenAI API key")
        } catch {
            XCTFail("Unexpected error: \(error)")
        }
    }
}
