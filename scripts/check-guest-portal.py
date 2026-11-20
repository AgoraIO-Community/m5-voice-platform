from html.parser import HTMLParser
from http.cookiejar import CookieJar
from urllib.error import URLError
from urllib.parse import urljoin, urlsplit
from urllib.request import HTTPCookieProcessor, build_opener


class Forms(HTMLParser):
    def __init__(self, url):
        super().__init__()
        self.base = url
        self.page = url
        self.found = False

    def handle_starttag(self, tag, attrs):
        fields = dict(attrs)
        if tag == "base" and fields.get("href"):
            self.base = urljoin(self.page, fields["href"])
        elif tag == "form":
            self.found = True
            target = urlsplit(urljoin(self.base, fields["action"]) if fields.get("action") else self.page)
            print("Form destination:", target.scheme, target.hostname)
            print("Method:", (fields.get("method") or "GET").upper())
        elif tag == "input" and fields.get("name"):
            print("Field:", fields["name"], "(" + (fields.get("type") or "text") + ")")


def main():
    opener = build_opener(HTTPCookieProcessor(CookieJar()))
    try:
        with opener.open("http://example.com/", timeout=10) as response:
            url = response.geturl()
            print("Page host:", urlsplit(url).hostname)
            forms = Forms(url)
            forms.feed(response.read(512_000).decode("utf-8", errors="replace"))
            if not forms.found:
                print("No HTML form detected in this response.")
    except (URLError, OSError) as error:
        raise SystemExit("Portal check failed: " + type(error).__name__)


if __name__ == "__main__":
    main()
