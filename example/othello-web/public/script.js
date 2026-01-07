document.addEventListener('DOMContentLoaded', () => {
    const board = document.getElementById('board');
    let player_id = null;
    let room_id = 1;

    const urlParams = new URLSearchParams(window.location.search);
    const roomParam = urlParams.get('room');
    if (roomParam) {
        room_id = parseInt(roomParam, 10);
    }

    joinGame();

    board.addEventListener('click', async (event) => {
        const cell = event.target.closest('.cell');
        if (cell) {
            const r = parseInt(cell.dataset.r, 10);
            const c = parseInt(cell.dataset.c, 10);
            await makeMove(r, c);
        }
    });

    async function joinGame() {
        try {
            const response = await fetch(`/join?room=${room_id}`, { method: 'POST' });
            const data = await response.json();
            player_id = data.player_id;
            document.getElementById('room-id').textContent = data.room_id;
            document.getElementById('game-mode').textContent = data.mode;
            pollState();
        } catch (error) {
            console.error('Failed to join game:', error);
            document.getElementById('status-message').textContent = 'Error joining room.';
        }
    }

    async function makeMove(r, c) {
        try {
            await fetch(`/move?room=${room_id}`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ r, c, player: player_id })
            });
            // The state will be updated by the poller
        } catch (error) {
            console.error('Failed to make move:', error);
        }
    }

    async function pollState() {
        try {
            const response = await fetch(`/state?room=${room_id}`);
            const state = await response.json();
            updateBoard(state);
        } catch (error) {
            console.error('Failed to get state:', error);
        }
        setTimeout(pollState, 1000); // Poll every second
    }

    function updateBoard(state) {
        const boardElement = document.getElementById('board');
        boardElement.innerHTML = ''; 

        state.board.forEach((row, r) => {
            const rowElement = document.createElement('div');
            rowElement.className = 'row';
            row.forEach((cell, c) => {
                const cellElement = document.createElement('div');
                cellElement.className = 'cell';
                cellElement.dataset.r = r;
                cellElement.dataset.c = c;

                if (cell === 1) { // Black
                    const piece = document.createElement('div');
                    piece.className = 'piece black';
                    cellElement.appendChild(piece);
                } else if (cell === 2) { // White
                    const piece = document.createElement('div');
                    piece.className = 'piece white';
                    cellElement.appendChild(piece);
                }
                rowElement.appendChild(cellElement);
            });
            boardElement.appendChild(rowElement);
        });
        
        document.getElementById('status-message').textContent = state.status;
        let turnText = 'None';
        if(state.turn === 1) turnText = 'Black';
        if(state.turn === 2) turnText = 'White';
        document.getElementById('turn').textContent = turnText;
    }
});
