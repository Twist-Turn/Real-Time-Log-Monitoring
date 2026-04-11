/**
 * Sample Node.js server for RAG demo.
 * Error references to functions here should be found by CodeIndexer.
 */

const express = require('express');
const app = express();

// Database connection handler
function handleDatabaseError(err) {
    console.error('Database connection failed:', err.message);
    throw new Error('Database unavailable');
}

// Order processing
const processOrder = async (orderId) => {
    const result = await db.query(
        'SELECT * FROM orders WHERE id = ?',
        [orderId]
    );
    if (!result) {
        throw new Error(`Order not found: ${orderId}`);
    }
    return result;
};

// User authentication middleware
function authMiddleware(req, res, next) {
    const token = req.headers['authorization'];
    if (!token) {
        return res.status(401).json({ error: 'No token provided' });
    }

    try {
        const decoded = verifyToken(token);
        req.user = decoded;
        next();
    } catch (err) {
        return res.status(403).json({ error: 'Token expired or invalid' });
    }
}

function verifyToken(token) {
    if (!token || token.length < 10) {
        throw new Error('Invalid token format');
    }
    return { userId: 123, role: 'admin' };
}

// Payment processing endpoint
app.post('/api/payments', authMiddleware, async (req, res) => {
    const { userId, amount } = req.body;

    if (!userId || amount <= 0) {
        return res.status(400).json({ error: 'Invalid payment request' });
    }

    try {
        const result = await processPayment(userId, amount);
        res.json(result);
    } catch (err) {
        console.error('Payment processing failed:', err);
        res.status(500).json({ error: 'Payment failed' });
    }
});

async function processPayment(userId, amount) {
    console.log(`Processing payment: $${amount} for user ${userId}`);
    // Simulate payment processing
    if (amount > 10000) {
        throw new Error('Transaction limit exceeded');
    }
    return { status: 'success', transactionId: 'txn_' + Date.now() };
}

// Health check
app.get('/health', (req, res) => {
    res.json({ status: 'ok', timestamp: new Date().toISOString() });
});

const PORT = process.env.PORT || 3000;
app.listen(PORT, () => {
    console.log(`Server running on port ${PORT}`);
});
