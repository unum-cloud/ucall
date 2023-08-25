from ucall.uring import Server
from ucall.server import Protocol
from typing import Optional

server = Server(
    port=8545,
    protocol=Protocol.REST,
    # ssl_pk='./examples/login/certs/main.key',
    # ssl_certs=[
    #   './examples/login/certs/srv.crt',
    #   './examples/login/certs/cas.pem']
)

books = dict()


@server.get('/books')
def book_list(starts_with: Optional[str] = None):
    if starts_with != None:
        return {'books': [book for book in books.values() if book.startswith(starts_with)]}
    return {'books': list(books.values())}


@server.get('/books/{book_id}')
def book_detail(book_id: str):
    book_id = int(book_id)
    if book_id in books:
        return {'book': book_id}
    else:
        raise RuntimeError('Book not found')


@server.post('/books')
def book_add(book_id: int, book_name: str):
    books[book_id] = book_name
    return {'book': books[book_id]}


@server.put('/books/{old_book_id}')
def book_update(book_name: str, old_book_id: str):
    old_book_id = int(old_book_id)
    if old_book_id in books.keys():
        books[old_book_id] = book_name
        return {'book': books[old_book_id]}
    else:
        raise RuntimeError('Book not found')


@server.delete('/books/{book_name}')
def book_delete(book_name: str):
    old_book_id = int(old_book_id)
    if old_book_id in books.keys():
        books.pop(old_book_id)
        return {'books': list(books.values())}
    else:
        raise RuntimeError('Book not found')


if __name__ == '__main__':
    server.run()
